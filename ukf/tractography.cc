/**
 * \file tractography.cc
 * \brief implementation of tractography.h
*/

#include <itkMacro.h> // needed for ITK_VERSION_MAJOR
#if ITK_VERSION_MAJOR >= 5
#include "itkMultiThreaderBase.h"
#include "itkPlatformMultiThreader.h"
#else

#include "itkMultiThreader.h"
#endif

// System includes
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <iomanip>

// VTK includes
#include "vtkNew.h"
#include "vtkPolyData.h"

// UKF includes
#include "tractography.h"
#include "filter_model.h"
#include "ISignalData.h"
#include "NrrdData.h"
#include "utilities.h"
#include "vtk_writer.h"
#include "thread.h"
#include "math_utilities.h"

// filters
#include "filter_ridg.h"

// TODO implement this switch
//#include "config.h"

Tractography::Tractography(UKFSettings s) : // begin initializer list
                                            _ukf(0, NULL),
                                            _output_file(s.output_file),
                                            _output_file_with_second_tensor(s.output_file_with_second_tensor),

                                            _record_nmse(s.record_nmse),
                                            _record_trace(s.record_trace),
                                            _record_state(s.record_state),
                                            _record_cov(s.record_cov),
                                            _record_free_water(s.record_free_water),
                                            _record_tensors(s.record_tensors),
                                            _record_weights(s.record_weights),
                                            _record_uncertainties(s.record_uncertainties),
                                            _transform_position(s.transform_position),
                                            _store_glyphs(s.store_glyphs),

                                            _p0(s.p0),
                                            _sigma_signal(s.sigma_signal),
                                            _sigma_mask(s.sigma_mask),
                                            _min_radius(s.min_radius),
                                            _max_length(static_cast<int>(std::ceil(s.maxHalfFiberLength / s.stepLength))),
                                            _full_brain(false),
                                            _is_seeds(false),
                                            _csf_provided(false),
                                            _wm_provided(false),
                                            _rtop1_min_stop(s.rtop1_min_stop),
                                            _record_rtop(s.record_rtop),
                                            _max_nmse(s.max_nmse),
                                            _maxUKFIterations(s.maxUKFIterations),
                                            _fw_thresh(s.fw_thresh),
                                            _fa_min(s.fa_min),
                                            _mean_signal_min(s.mean_signal_min),
                                            _seeding_threshold(s.seeding_threshold),
                                            _num_tensors(s.num_tensors),
                                            _seeds_per_voxel(s.seeds_per_voxel),
                                            _stepLength(s.stepLength),
                                            _steps_per_record(s.recordLength / s.stepLength),
                                            _labels(s.labels),

                                            Qm(s.Qm),
                                            Ql(s.Ql),
                                            Qw(s.Qw),
                                            Qt(s.Qt),
                                            Qwiso(s.Qwiso),
                                            Qkappa(s.Qkappa),
                                            Qvic(s.Qvic),
                                            Rs(s.Rs),

                                            _writeBinary(true),
                                            _writeCompressed(true),

                                            _num_threads(s.num_threads),
                                            _outputPolyData(NULL),

                                            _model(NULL),
                                            debug(false),
                                            sph_rho(3.125),
                                            sph_J(2),
                                            fista_lambda(0.01),
                                            lvl(4),
                                            max_odf_thresh(s.max_odf_threshold)
// end initializer list
{
}

Tractography::~Tractography()
{
  if (this->_signal_data)
  {
    delete this->_signal_data;
    this->_signal_data = NULL;
  }
  if (this->_model)
  {
    delete this->_model; // TODO smartpointer
    this->_model = NULL;
  }
}

void Tractography::UpdateFilterModelType()
{
  if (!this->_signal_data)
  {
    return;
  }

  if (this->_model)
  {
    delete this->_model; // TODO smartpointer
    this->_model = NULL;
  }

  _num_tensors = 3;
  _nPosFreeWater = 24;

  // 0.1.1. Compute ridgelets basis...
  // but first convert gradients to ukfMatrixType
  const int s_dim = _signal_data->GetSignalDimension() * 2;
  ukfMatrixType GradientDirections(s_dim, 3);
  const stdVec_t &gradients = _signal_data->gradients();
  for (int j = 0; j < s_dim; ++j)
  {
    const vec3_t &u = gradients[j];
    GradientDirections.row(j) = u;
  }

  // Get indicies of voxels in a range
  const ukfVectorType b_vals = _signal_data->GetBValues();
  const ukfPrecisionType nominal_b_val = _signal_data->GetNominalBValue();

  int vx = 0;
  for (int i = 0; i < b_vals.size() / 2; ++i)
  {
    if (b_vals(i) >= (nominal_b_val - 150) && b_vals(i) <= (nominal_b_val + 150))
    {
      signal_mask.conservativeResize(signal_mask.size() + 1);
      signal_mask(vx) = i;
      vx++;
    }
  }

  //Take only highest b-value gradient directions
  ukfMatrixType HighBGradDirss(signal_mask.size(), 3);
  for (int indx = 0; indx < signal_mask.size(); ++indx)
    HighBGradDirss.row(indx) = GradientDirections.row(signal_mask(indx));

  // Compute A basis
  // Spherical Ridgelets helper functions
  UtilMath<ukfPrecisionType, ukfMatrixType, ukfVectorType> m;
  SPH_RIDG<ukfPrecisionType, ukfMatrixType, ukfVectorType> ridg(sph_J, 1 / sph_rho);

  ridg.RBasis(ARidg, HighBGradDirss);
  ridg.normBasis(ARidg);

  // Compute Q basis
  m.icosahedron(nu, fcs, lvl);
  ridg.QBasis(QRidg, nu); //Build a Q basis
  ridg.QBasis(QRidgSignal, HighBGradDirss);

  // Compute connectivity
  m.FindConnectivity(conn, fcs, nu.rows());

  _model = new Ridg_BiExp_FW(Qm, Ql, Qt, Qw, Qwiso, Rs, true, D_ISO, ARidg, QRidg, fcs, nu, conn, signal_mask, fista_lambda, max_odf_thresh);

  _model->set_signal_data(_signal_data);
  _model->set_signal_dim(_signal_data->GetSignalDimension() * 2);
}

bool Tractography::SetData(void *data, void *mask, void *csf, void *wm,
                           void *seed, bool normalizedDWIData)
{
  if (!data || !mask)
  {
    std::cout << "Invalid input Nrrd pointers!" << std::endl;
    return true;
  }

  if (!seed)
    _full_brain = true;
  else
    _is_seeds = true;

  if (!csf)
    _csf_provided = false;
  else
    _csf_provided = true;

  if (!wm)
  {
    _wm_provided = false;
    _full_brain = true;
  }
  else
  {
    _wm_provided = true;
    _full_brain = false;
  }

  _signal_data = new NrrdData(_sigma_signal, _sigma_mask);
  _signal_data->SetData((Nrrd *)data, (Nrrd *)mask, (Nrrd *)csf, (Nrrd *)wm, (Nrrd *)seed, normalizedDWIData);

  return false;
}

bool Tractography::LoadFiles(const std::string &data_file,
                             const std::string &seed_file,
                             const std::string &mask_file,
                             const std::string &csf_file,
                             const std::string &wm_file,
                             const bool normalized_DWI_data,
                             const bool output_normalized_DWI_data)
{
  _signal_data = new NrrdData(_sigma_signal, _sigma_mask);

  if (seed_file.empty())
    _full_brain = true;
  else
    _is_seeds = true;

  if (csf_file.empty())
    _csf_provided = false;
  else
    _csf_provided = true;

  if (wm_file.empty())
  {
    _wm_provided = false;
    _full_brain = true;
  }
  else
  {
    _wm_provided = true;
    _full_brain = false;
  }

  if (_signal_data->LoadData(data_file, seed_file, mask_file, csf_file, wm_file, normalized_DWI_data, output_normalized_DWI_data))
  {
    std::cout << "ISignalData could not be loaded" << std::endl;
    delete _signal_data;
    _signal_data = NULL;
    return true;
  }
  return false;
}

void Tractography::Init(std::vector<SeedPointInfo> &seed_infos)
{
  if (!(_signal_data))
  {
    std::cout << "No signal data!";
    throw;
  }

  if (_is_seeds)
    std::cout << "Seed file Provided!\n";
  else
    std::cout << "Seed file is NOT provided!\n";

  if (_csf_provided)
    std::cout << "CSF Provided!\n";
  else
    std::cout << "CSF is NOT provided!\n";

  if (_wm_provided)
    std::cout << "WM Provided!\n";
  else
    std::cout << "WM is NOT provided!\n";

  int signal_dim = _signal_data->GetSignalDimension();

  stdVec_t seeds;
  if (!(_labels.size() > 0))
  {
    std::cout << "No label data!";
    throw;
  }

  if (!_ext_seeds.empty())
  {
    seeds = _ext_seeds;
  }
  else if (_is_seeds)
  {
    _signal_data->GetSeeds(_labels, seeds);
  }
  else if (_wm_provided)
  {
    _signal_data->GetWMSeeds(seeds);
  }
  else
  {
    // Iterate through all brain voxels and take those as seeds voxels.
    const vec3_t dim = _signal_data->dim();
    for (int x = 0; x < dim[0]; ++x)
    {
      for (int y = 0; y < dim[1]; ++y)
      {
        for (int z = 0; z < dim[2]; ++z)
        {
          vec3_t pos(x, y, z); //  = make_vec(x, y, z);
          if (_signal_data->ScalarMaskValue(pos) > 0)
            seeds.push_back(pos);
        }
      }
    }
  }

  if (!(seeds.size() > 0))
  {
    std::cout << "No matching label ROI seeds found! Please verify label selection!";
    throw;
  }

  // Determinism.
  srand(0);

  // Create random offsets from the seed voxel.
  stdVec_t rand_dirs;

  if ((seeds.size() == 1 && _seeds_per_voxel <= 1.0) || _seeds_per_voxel <= 1.0) // if there is only one seed don't use offset so fibers can be
                                                                                 // compared
  {
    rand_dirs.push_back(vec3_t(0, 0, 0) /* make_vec(0, 0, 0) */); // in the test cases.
  }
  else
  {
    for (int i = 0; i < _seeds_per_voxel; ++i)
    {
      vec3_t dir(static_cast<ukfPrecisionType>((rand() % 10001) - 5000),
                 static_cast<ukfPrecisionType>((rand() % 10001) - 5000),
                 static_cast<ukfPrecisionType>((rand() % 10001) - 5000));

      // CB: those directions are to compare against the matlab output
      // dir._[2] = 0.439598093988175;
      // dir._[1] = 0.236539281163321;
      // dir._[0] = 0.028331682419209;

      dir = dir.normalized();
      dir *= ukfHalf;

      rand_dirs.push_back(dir);
    }
  }

  // Calculate all starting points.
  stdVec_t starting_points;
  stdEigVec_t signal_values;
  ukfVectorType signal(signal_dim * 2);

  int num_less_than_zero = 0;
  int num_invalid = 0;

  int tmp_counter = 1;
  unsigned every_n = 1;

  if (_seeds_per_voxel < 1.0)
  {
    every_n = static_cast<unsigned>(1.0 / _seeds_per_voxel); // will be rounded to the nearest int
    std::cout << "Seed every " << every_n << " point" << std::endl;
  }

  for (stdVec_t::const_iterator cit = seeds.begin(); cit != seeds.end(); ++cit)
  {
    for (stdVec_t::iterator jt = rand_dirs.begin(); jt != rand_dirs.end(); ++jt)
    {
      if (tmp_counter % every_n == 0)
      {
        vec3_t point = *cit + *jt;

        _signal_data->Interp3Signal(point, signal); // here and in every step

        // Filter out all starting points that have negative signal values (due to
        // noise) or that otherwise have invalid signal values.
        bool keep = true;
        // We only scan half of the signal values since the second half is simply
        // a copy of the first half.
        for (int k = 0; k < signal_dim; ++k)
        {
          if (signal[k] < 0)
          {
            keep = false;
            ++num_less_than_zero;
            break;
          }

          if (std::isnan(signal[k]) || std::isinf(signal[k]))
          {
            keep = false;
            ++num_invalid;
            break;
          }
        }

        // If all the criteria is met we keep that point and the signal data.
        if (keep)
        {
          signal_values.push_back(signal);
          starting_points.push_back(point);
        }
      }

      tmp_counter++;
    }
  }

  stdEigVec_t starting_params(starting_points.size());

  UnpackTensor(_signal_data->GetBValues(), _signal_data->gradients(),
               signal_values, starting_params);

  // If we work with the simple model we have to change the second and third
  // eigenvalues: l2 = l3 = (l2 + l3) / 2.
  for (size_t i = 0; i < starting_params.size(); ++i)
  {
    starting_params[i][7] = starting_params[i][8] = (starting_params[i][7] + starting_params[i][8]) / 2.0;
    // two minor eigenvalues are treated equal in simplemodel
  }

#if defined(_OPENMP)
  const int num_of_threads = std::min(_num_threads, static_cast<int>(starting_points.size()));
  omp_set_num_threads(num_of_threads);

  std::cout << "Processing " << starting_points.size() << " starting points with " << num_of_threads << " threads" << std::endl;

#pragma omp parallel for
#else
  std::cout << "Multithreading for seed points initialization is not available!\n "
               "Please, compile the UKF tractography software with OpenMP support enabled if you want this functionality."
            << std::ednl;
#endif
  for (unsigned i = 0; i < starting_points.size(); ++i)
  {
    const ukfVectorType &param = starting_params[i];

    // Filter out seeds whose FA is too low.
    ukfPrecisionType fa = l2fa(param[6], param[7], param[8]);
    ukfPrecisionType trace = param[6] + param[7] + param[8];
    ukfPrecisionType fa2 = -1;
    ukfPrecisionType fa3 = -1;
    ukfPrecisionType trace2 = -1;

    if (_num_tensors >= 2)
    {
      fa2 = fa;
      fa3 = fa;
      trace2 = trace;
    }

    // Create seed info for both directions;
    SeedPointInfo info;
    stdVecState tmp_info_state;
    stdVecState tmp_info_inv_state;
    SeedPointInfo info_inv;

    info.point = starting_points[i];
    info.start_dir << param[0], param[1], param[2];
    info.fa = fa;
    info.fa2 = fa2;
    info.fa3 = fa3;
    info.trace = trace;
    info.trace2 = trace2;
    info_inv.point = starting_points[i];
    info_inv.start_dir << -param[0], -param[1], -param[2];
    info_inv.fa = fa;
    info_inv.fa2 = fa2;
    info_inv.fa3 = fa3;
    info_inv.trace = trace;
    info_inv.trace2 = trace2;

    tmp_info_state.resize(25);
    tmp_info_inv_state.resize(25);

    // STEP 0: Find number of branches in one voxel.

    // Compute number of branches at the seed point using spherical ridgelets
    UtilMath<ukfPrecisionType, ukfMatrixType, ukfVectorType> m;

    ukfVectorType HighBSignalValues(signal_mask.size());
    for (int indx = 0; indx < signal_mask.size(); ++indx)
      HighBSignalValues(indx) = signal_values[i](signal_mask(indx));

    // We can compute ridegelets coefficients
    ukfVectorType C;
    {
      SOLVERS<ukfPrecisionType, ukfMatrixType, ukfVectorType> slv(ARidg, HighBSignalValues, fista_lambda);
      slv.FISTA(C);
    }

    ukfPrecisionType GFA = 0.0;
    if (_full_brain)
      GFA = s2ga(QRidgSignal * C);

    if (GFA > _seeding_threshold || !_full_brain || _is_seeds)
    {
      // Now we can compute ODF
      ukfVectorType ODF = QRidg * C;

      // Let's find Maxima of ODF and values in that direction
      ukfMatrixType exe_vol;
      ukfMatrixType dir_vol;
      ukfVectorType ODF_val_at_max(6, 1);
      unsigned n_of_dirs;

      m.FindODFMaxima(exe_vol, dir_vol, ODF, conn, nu, max_odf_thresh, n_of_dirs);

      unsigned exe_vol_size = std::min(static_cast<unsigned>(exe_vol.size()), static_cast<unsigned>(6));
      ODF_val_at_max.setZero();
      for (unsigned j = 0; j < exe_vol_size; ++j)
      {
        ODF_val_at_max(j) = ODF(exe_vol(j));
      }

      // STEP 1: Initialise the state based
      mat33_t dir_init;
      dir_init.setZero();

      ukfPrecisionType w1_init = ODF_val_at_max(0);
      dir_init.row(0) = dir_vol.row(0);

      ukfPrecisionType w2_init = 0;
      ukfPrecisionType w3_init = 0;

      //std::cout << "n_of_dirs " << n_of_dirs << std::endl;

      if (n_of_dirs == 1)
      {
        vec3_t orthogonal;
        orthogonal << -dir_vol.row(0)[1], dir_vol.row(0)[0], 0.0;
        orthogonal = orthogonal / orthogonal.norm();
        dir_init.row(1) = orthogonal;

        vec3_t orthogonal2 = dir_init.row(0).cross(orthogonal);
        orthogonal2 = orthogonal2 / orthogonal2.norm();
        dir_init.row(2) = orthogonal2;

        w1_init = 1.0;
      }
      else if (n_of_dirs > 1)
      {
        if (n_of_dirs == 2)
        {
          vec3_t v1 = dir_vol.row(0);
          vec3_t v2 = dir_vol.row(2);
          vec3_t orthogonal = v1.cross(v2);
          orthogonal = orthogonal / orthogonal.norm();

          dir_init.row(1) = dir_vol.row(2);
          dir_init.row(2) = orthogonal;

          w2_init = ODF_val_at_max(2);
          ukfPrecisionType denom = w1_init + w2_init;
          w1_init = w1_init / denom;
          w2_init = w2_init / denom;
        }
        if (n_of_dirs > 2)
        {
          dir_init.row(1) = dir_vol.row(2);
          dir_init.row(2) = dir_vol.row(4);

          w2_init = ODF_val_at_max(2);
          w3_init = ODF_val_at_max(4);
          ukfPrecisionType denom = w1_init + w2_init + w3_init;
          w1_init = w1_init / denom;
          w2_init = w2_init / denom;
          w3_init = w3_init / denom;
        }
      }

      // Diffusion directions, m1 = m2 = m3
      tmp_info_state[0] = dir_init.row(0)[0];
      tmp_info_state[1] = dir_init.row(0)[1];
      tmp_info_state[2] = dir_init.row(0)[2];

      tmp_info_state[7] = dir_init.row(1)[0];
      tmp_info_state[8] = dir_init.row(1)[1];
      tmp_info_state[9] = dir_init.row(1)[2];

      tmp_info_state[14] = dir_init.row(2)[0];
      tmp_info_state[15] = dir_init.row(2)[1];
      tmp_info_state[16] = dir_init.row(2)[2];

      // Fast diffusing component,  lambdas l11, l21 = l1 from the single tensor
      //                            lambdas l12, l21 = (l2 + l3) /2
      // from the single tensor (avg already calculated and stored in l2)
      tmp_info_state[3] = tmp_info_state[10] = tmp_info_state[17] = param[6];
      tmp_info_state[4] = tmp_info_state[11] = tmp_info_state[18] = param[7];

      // Slow diffusing component,  lambdas l13, l23, l33 = 0.2 * l1
      //                            lambdas l14, l24, l34 = 0.2 * (l2 + l3) /2
      tmp_info_state[5] = tmp_info_state[12] = tmp_info_state[19] = 0.7 * param[6];
      tmp_info_state[6] = tmp_info_state[13] = tmp_info_state[20] = 0.7 * param[7];

      tmp_info_state[21] = w1_init;
      tmp_info_state[22] = w2_init;
      tmp_info_state[23] = w3_init;

      // Free water volume fraction
      tmp_info_state[24] = 0.05; // -> as an initial value

      // STEP 2.1: Use L-BFGS-B from ITK library at the same point in space.
      // The UKF is an estimator, and we want to find the estimate with the smallest error through the iterations

      // Set the covariance value
      const int state_dim = tmp_info_state.size();
      info.covariance.resize(state_dim, state_dim);
      info_inv.covariance.resize(state_dim, state_dim);

      // make sure covariances are really empty
      info.covariance.setConstant(ukfZero);
      info_inv.covariance.setConstant(ukfZero);

      // fill the diagonal of the covariance matrix with _p0 (zeros elsewhere)
      for (int local_i = 0; local_i < state_dim; ++local_i)
      {
        info.covariance(local_i, local_i) = _p0;
        info_inv.covariance(local_i, local_i) = _p0;
      }

      // Input of the filter
      State state = ConvertVector<stdVecState, State>(tmp_info_state);
      ukfMatrixType p(info.covariance);

      // Estimate the initial state
      // InitLoopUKF(state, p, signal_values[i], dNormMSE);
      NonLinearLeastSquareOptimization(state, signal_values[i]);

      // Output of the filter
      tmp_info_state = ConvertVector<State, stdVecState>(state);

      ukfPrecisionType rtopModel = 0.0;
      ukfPrecisionType rtop1 = 0.0;
      ukfPrecisionType rtop2 = 0.0;
      ukfPrecisionType rtop3 = 0.0;
      ukfPrecisionType rtopSignal = 0.0;

      computeRTOPfromState(state, rtopModel, rtop1, rtop2, rtop3);
      computeRTOPfromSignal(rtopSignal, signal_values[i]);

      // These values are stored so that: rtop1 -> fa; rtop2 -> fa2; rtop3 -> fa3; rtop -> trace; rtopSignal -> trace2
      info.fa = rtop1;
      info.fa2 = rtop2;
      info.fa3 = rtop3;
      info.trace = rtopModel;
      info.trace2 = rtopSignal;

      info_inv.fa = rtop1;
      info_inv.fa2 = rtop2;
      info_inv.fa3 = rtop3;
      info_inv.trace = rtopModel;
      info_inv.trace2 = rtopSignal;

      // Create the opposite seed
      InverseStateDiffusionPropagator(tmp_info_state, tmp_info_inv_state);

      // Update the original directions
      info.start_dir << tmp_info_state[0], tmp_info_state[1], tmp_info_state[2];
      info_inv.start_dir << -tmp_info_state[0], -tmp_info_state[1], -tmp_info_state[2];

      // Add the primary seeds to the vector
      info.state = ConvertVector<stdVecState, State>(tmp_info_state);
      info_inv.state = ConvertVector<stdVecState, State>(tmp_info_inv_state);

      seed_infos.push_back(info);
      seed_infos.push_back(info_inv);

      if (n_of_dirs > 1)
      {
        SwapState(tmp_info_state, p, 2);
        info.start_dir << tmp_info_state[0], tmp_info_state[1], tmp_info_state[2];
        info.state = ConvertVector<stdVecState, State>(tmp_info_state);

        // Create the seed for the opposite direction, keep the other parameters as set for the first direction
        InverseStateDiffusionPropagator(tmp_info_state, tmp_info_inv_state);

        info_inv.state = ConvertVector<stdVecState, State>(tmp_info_inv_state);
        info_inv.start_dir << tmp_info_inv_state[0], tmp_info_inv_state[1], tmp_info_inv_state[2];

        seed_infos.push_back(info);
        seed_infos.push_back(info_inv);

        if (n_of_dirs > 2)
        {
          SwapState(tmp_info_state, p, 3);
          info.start_dir << tmp_info_state[0], tmp_info_state[1], tmp_info_state[2];
          info.state = ConvertVector<stdVecState, State>(tmp_info_state);

          // Create the seed for the opposite direction, keep the other parameters as set for the first direction
          InverseStateDiffusionPropagator(tmp_info_state, tmp_info_inv_state);

          info_inv.state = ConvertVector<stdVecState, State>(tmp_info_inv_state);
          info_inv.start_dir << tmp_info_inv_state[0], tmp_info_inv_state[1], tmp_info_inv_state[2];

          seed_infos.push_back(info);
          seed_infos.push_back(info_inv);
        }
      }
    }
  }
  std::cout << "Final seeds vector size " << seed_infos.size() << std::endl;
}

bool Tractography::Run()
{
  assert(_signal_data); // The _signal_data is initialized in Tractography::LoadFiles(),
  // Thus Run() must be invoked after LoadFiles()
  // Initialize and prepare seeds.

  std::vector<SeedPointInfo> primary_seed_infos;

  Init(primary_seed_infos);
  if (primary_seed_infos.size() < 1)
  {
    std::cerr << "No valid seed points available!" << std::endl;
    return false;
  }

  const int num_of_threads = std::min(_num_threads, static_cast<int>(primary_seed_infos.size()));

  assert(num_of_threads > 0);

  _ukf.reserve(num_of_threads); //Allocate, but do not assign
  for (int i = 0; i < num_of_threads; i++)
  {
    _ukf.push_back(new UnscentedKalmanFilter(_model)); // Create one Kalman filter for each thread
  }

  std::vector<UKFFiber> raw_primary;
  std::vector<unsigned char> discarded_fibers;

  // Output directions
  // std::vector<UKFFiber> raw_primary_w1;
  // std::vector<UKFFiber> raw_primary_w2;
  // std::vector<UKFFiber> raw_primary_w3;

  {
    if (this->debug)
      std::cout << "Tracing " << primary_seed_infos.size() << " primary fibers:" << std::endl;

    raw_primary.resize(primary_seed_infos.size());
    discarded_fibers.resize(primary_seed_infos.size());
    // Output directions
    // raw_primary_w1.resize(primary_seed_infos.size() * 2);
    // raw_primary_w2.resize(primary_seed_infos.size() * 2);
    // raw_primary_w3.resize(primary_seed_infos.size() * 2);

    WorkDistribution work_distribution = GenerateWorkDistribution(num_of_threads,
                                                                  static_cast<int>(primary_seed_infos.size()));
#if ITK_VERSION_MAJOR >= 5
    itk::PlatformMultiThreader::Pointer threader = itk::PlatformMultiThreader::New();
    threader->SetNumberOfWorkUnits(num_of_threads);
    std::vector<std::thread> vectorOfThreads;
    vectorOfThreads.reserve(num_of_threads);
#else
    itk::MultiThreader::Pointer threader = itk::MultiThreader::New();
    threader->SetNumberOfThreads(num_of_threads);
#endif
    thread_struct str;
    str.tractography_ = this;
    str.seed_infos_ = &primary_seed_infos;
    str.work_distribution = &work_distribution;
    str.output_fiber_group_ = &raw_primary;
    str.discarded_fibers_ = &discarded_fibers;

    // Output directions
    // str.output_fiber_group_1_ = &raw_primary_w1;
    // str.output_fiber_group_2_ = &raw_primary_w2;
    // str.output_fiber_group_3_ = &raw_primary_w3;

    for (int i = 0; i < num_of_threads; i++)
    {
#if ITK_VERSION_MAJOR >= 5
      vectorOfThreads.push_back(std::thread(ThreadCallback, i, &str));
#else
      threader->SetMultipleMethod(i, ThreadCallback, &str);
#endif
    }
#if ITK_VERSION_MAJOR < 5
    threader->SetGlobalDefaultNumberOfThreads(num_of_threads);
#else
    itk::MultiThreaderBase::SetGlobalDefaultNumberOfThreads(num_of_threads);
#endif

#if ITK_VERSION_MAJOR >= 5
    for (auto &li : vectorOfThreads)
    {
      if (li.joinable())
      {
        li.join();
      }
    }
#else
    threader->MultipleMethodExecute();
#endif
  }

  std::vector<UKFFiber> fibers;
  PostProcessFibers(raw_primary, discarded_fibers, fibers);

  if (this->debug)
    std::cout << "fiber size after PostProcessFibers: " << fibers.size() << std::endl;

  if (fibers.size() == 0)
  {
    std::cout << "No fibers! Returning." << fibers.size() << std::endl;
    return EXIT_FAILURE;
  }

  // Write the fiber data to the output vtk file.
  VtkWriter writer(_signal_data, _record_tensors);
  writer.set_transform_position(_transform_position);

  int writeStatus = EXIT_SUCCESS;
  if (this->_outputPolyData != NULL)
  // TODO refactor this is bad control flow
  {
    writer.PopulateFibersAndTensors(this->_outputPolyData, fibers);
    this->_outputPolyData->Modified();
  }
  else
  {
    vtkNew<vtkPolyData> pd;
    this->SetOutputPolyData(pd.GetPointer());
    writer.PopulateFibersAndTensors(this->_outputPolyData, fibers);
    this->_outputPolyData->Modified();

    // possibly write binary VTK file.
    writer.SetWriteBinary(this->_writeBinary);
    writer.SetWriteCompressed(this->_writeCompressed);

    writeStatus = writer.Write(_output_file, fibers, _record_state, _store_glyphs);

    // Output directions
    // std::string out_dir = _output_file;
    // out_dir.erase(_output_file.length() - 4);
    // writeStatus = writer.WriteWeight(out_dir + "_dir1.vtk", raw_primary_w1);
    // writeStatus = writer.WriteWeight(out_dir + "_dir2.vtk", raw_primary_w2);
    // writeStatus = writer.WriteWeight(out_dir + "_dir3.vtk", raw_primary_w3);
    // TODO refactor!
    this->SetOutputPolyData(NULL);
  }

  // Clear up the kalman filters
  for (size_t i = 0; i < _ukf.size(); i++)
  {
    delete _ukf[i];
    _ukf[i] = NULL;
  }
  _ukf.clear();
  return writeStatus;
}

void Tractography::computeRTOPfromSignal(ukfPrecisionType &rtopSignal, const ukfVectorType &signal)
{
  //assert(signal.size() > 0);

  rtopSignal = 0.0;

  // The RTOP is the sum of the signal
  // We use signal.size()/2 because the first half of the signal is identical
  // to the second half.

  for (int i = 0; i < signal.size() / 2; ++i)
  {
    rtopSignal += signal[i];

    if (signal[i] < 0)
    {
      std::cout << "Negative signal found when computing the RTOP from the signal, value : " << signal[i] << std::endl;
    }
  }
}

void Tractography::computeUncertaintiesCharacteristics(ukfMatrixType &cov,
                                                       ukfPrecisionType &Fm1,
                                                       ukfPrecisionType &lmd1,
                                                       ukfPrecisionType &Fm2,
                                                       ukfPrecisionType &lmd2,
                                                       ukfPrecisionType &Fm3,
                                                       ukfPrecisionType &lmd3,
                                                       ukfPrecisionType &varW1,
                                                       ukfPrecisionType &varW2,
                                                       ukfPrecisionType &varW3,
                                                       ukfPrecisionType &varWiso)
{
  Fm1 = (cov.block<3, 3>(0, 0)).norm();
  lmd1 = (cov.block<4, 4>(3, 3)).norm();
  Fm2 = (cov.block<3, 3>(7, 7)).norm();
  lmd2 = (cov.block<4, 4>(10, 10)).norm();
  Fm3 = (cov.block<3, 3>(14, 14)).norm();
  lmd3 = (cov.block<4, 4>(17, 17)).norm();
  varW1 = cov(21, 21);
  varW2 = cov(22, 22);
  varW3 = cov(23, 23);
  varWiso = cov(24, 24);
}

void Tractography::computeRTOPfromState(State &state, ukfPrecisionType &rtop, ukfPrecisionType &rtop1, ukfPrecisionType &rtop2, ukfPrecisionType &rtop3)
{
  state[3] = std::max(state[3], 1.0);
  state[4] = std::max(state[4], 1.0);
  state[5] = std::max(state[5], 0.1);
  state[6] = std::max(state[6], 0.1);

  state[3] = std::min(state[3], 3000.0);
  state[4] = std::min(state[4], 3000.0);
  state[5] = std::min(state[5], 3000.0);
  state[6] = std::min(state[6], 3000.0);

  state[10] = std::max(state[10], 1.0);
  state[11] = std::max(state[11], 1.0);
  state[12] = std::max(state[12], 0.1);
  state[13] = std::max(state[13], 0.1);

  state[10] = std::min(state[10], 3000.0);
  state[11] = std::min(state[11], 3000.0);
  state[12] = std::min(state[12], 3000.0);
  state[13] = std::min(state[13], 3000.0);

  state[17] = std::max(state[17], 1.0);
  state[18] = std::max(state[18], 1.0);
  state[19] = std::max(state[19], 0.1);
  state[20] = std::max(state[20], 0.1);

  state[17] = std::min(state[17], 3000.0);
  state[18] = std::min(state[18], 3000.0);
  state[19] = std::min(state[19], 3000.0);
  state[20] = std::min(state[20], 3000.0);

  state[21] = std::max(state[21], 0.0);
  state[22] = std::max(state[22], 0.0);
  state[23] = std::max(state[23], 0.0);
  state[24] = std::max(state[24], 0.0);

  state[21] = std::min(state[21], 1.0);
  state[22] = std::min(state[22], 1.0);
  state[23] = std::min(state[23], 1.0);
  state[24] = std::min(state[24], 1.0);

  // Control input: state should have 25 rows
  //assert(state.size() == 25);

  ukfPrecisionType l11 = state[3] * 1e-6;
  ukfPrecisionType l12 = state[4] * 1e-6;
  ukfPrecisionType l13 = state[5] * 1e-6;
  ukfPrecisionType l14 = state[6] * 1e-6;

  ukfPrecisionType l21 = state[10] * 1e-6;
  ukfPrecisionType l22 = state[11] * 1e-6;
  ukfPrecisionType l23 = state[12] * 1e-6;
  ukfPrecisionType l24 = state[13] * 1e-6;

  ukfPrecisionType l31 = state[17] * 1e-6;
  ukfPrecisionType l32 = state[18] * 1e-6;
  ukfPrecisionType l33 = state[19] * 1e-6;
  ukfPrecisionType l34 = state[20] * 1e-6;

  ukfPrecisionType w1 = state[21];
  ukfPrecisionType w2 = state[22];
  ukfPrecisionType w3 = state[23];
  ukfPrecisionType wiso = state[24];

  ukfPrecisionType det_l1 = l11 * l12;
  ukfPrecisionType det_t1 = l13 * l14;

  ukfPrecisionType det_l2 = l21 * l22;
  ukfPrecisionType det_t2 = l23 * l24;

  ukfPrecisionType det_l3 = l31 * l32;
  ukfPrecisionType det_t3 = l33 * l34;

  ukfPrecisionType det_fw = D_ISO * D_ISO * D_ISO;

  ukfPrecisionType PI_COEFF = std::pow(UKF_PI, 1.5);

  // !!! 0.7 and 0.3 tensor weights are hardcoded...
  rtop1 = PI_COEFF * w1 * (0.7 / std::sqrt(det_l1) + 0.3 / std::sqrt(det_t1));
  rtop2 = PI_COEFF * w2 * (0.7 / std::sqrt(det_l2) + 0.3 / std::sqrt(det_t2));
  rtop3 = PI_COEFF * w3 * (0.7 / std::sqrt(det_l3) + 0.3 / std::sqrt(det_t3));
  rtop = rtop1 + rtop2 + rtop3 + PI_COEFF * (wiso / std::sqrt(det_fw));
}

void Tractography::PrintState(State &state)
{
  std::cout << "State \n";
  std::cout << "\t m1: " << state[0] << " " << state[1] << " " << state[2] << std::endl;
  std::cout << "\t l11 .. l14: " << state[3] << " " << state[4] << " " << state[5] << " " << state[6] << std::endl;
  std::cout << "\t m2: " << state[7] << " " << state[8] << " " << state[9] << std::endl;
  std::cout << "\t l21 .. l24: " << state[10] << " " << state[11] << " " << state[12] << " " << state[13] << std::endl;
  std::cout << "\t m3: " << state[14] << " " << state[15] << " " << state[16] << std::endl;
  std::cout << "\t l31 .. l34: " << state[17] << " " << state[18] << " " << state[19] << " " << state[20] << std::endl;
  std::cout << "\t w1, w2, w3: " << state[21] << " " << state[22] << "" << state[23] << std::endl;
  std::cout << "\t wiso: " << state[24] << std::endl;
  std::cout << " --- " << std::endl;
}

void Tractography::NonLinearLeastSquareOptimization(State &state, const ukfVectorType &signal)
{
  // Fill in array of parameters we are not intented to optimized
  // We still need to pass this parameters to optimizer because we need to compute
  // estimated signal during optimization and it requireds full state
  ukfVectorType fixed_params(12);
  fixed_params(0) = state(0);
  fixed_params(1) = state(1);
  fixed_params(2) = state(2);
  fixed_params(3) = state(7);
  fixed_params(4) = state(8);
  fixed_params(5) = state(9);
  fixed_params(6) = state(14);
  fixed_params(7) = state(15);
  fixed_params(8) = state(16);

  fixed_params(9) = state(21);
  fixed_params(10) = state(22);
  fixed_params(11) = state(23);

  ukfVectorType state_temp(13);
  state_temp(0) = state(3);
  state_temp(1) = state(4);
  state_temp(2) = state(5);
  state_temp(3) = state(6);
  state_temp(4) = state(10);
  state_temp(5) = state(11);
  state_temp(6) = state(12);
  state_temp(7) = state(13);
  state_temp(8) = state(17);
  state_temp(9) = state(18);
  state_temp(10) = state(19);
  state_temp(11) = state(20);

  state_temp(12) = state(24);

  // Lower bound
  ukfVectorType lowerBound(13);
  // First bi-exponential parameters
  lowerBound[0] = lowerBound[1] = 1.0;
  lowerBound[2] = lowerBound[3] = 0.1;

  // Second bi-exponential
  lowerBound[4] = lowerBound[5] = 1.0;
  lowerBound[6] = lowerBound[7] = 0.1;

  // Third bi-exponential
  lowerBound[8] = lowerBound[9] = 1.0;
  lowerBound[10] = lowerBound[11] = 0.1;

  // w1 & w2 & w3 in [0,1]
  //lowerBound[12] = lowerBound[13] = lowerBound[14] = 0.0;
  // free water between 0 and 1
  //lowerBound[15] = 0.0;
  lowerBound[12] = 0.0;

  // Upper bound
  ukfVectorType upperBound(13);
  // First bi-exponential
  upperBound[0] = upperBound[1] = upperBound[2] = upperBound[3] = 3000.0;

  // Second bi-exponential
  upperBound[4] = upperBound[5] = upperBound[6] = upperBound[7] = 3000.0;

  // Third bi-exponential
  upperBound[8] = upperBound[9] = upperBound[10] = upperBound[11] = 3000.0;

  //upperBound[12] = upperBound[13] = upperBound[14] = 1.0;
  //upperBound[15] = 1.0;
  upperBound[12] = 1.0;

  // init solver with bounds
  LFBGSB *_lbfgsb = new LFBGSB(_model);
  _lbfgsb->setSignal(signal);
  _lbfgsb->setFixed(fixed_params);
  _lbfgsb->setLowerBound(lowerBound);
  _lbfgsb->setUpperBound(upperBound);
  _lbfgsb->setPhase(1);
  // Run solver
  _lbfgsb->Solve(state_temp);

  state_temp = _lbfgsb->XOpt;
  //cout << "after " << state_temp.transpose() << endl << endl;
  //exit(0);

  //MySolver.XOpt;

  // Fill back the state tensor to return it the callee
  state(0) = fixed_params(0);
  state(1) = fixed_params(1);
  state(2) = fixed_params(2);
  state(7) = fixed_params(3);
  state(8) = fixed_params(4);
  state(9) = fixed_params(5);
  state(14) = fixed_params(6);
  state(15) = fixed_params(7);
  state(16) = fixed_params(8);

  state(21) = fixed_params(9);
  state(22) = fixed_params(10);
  state(23) = fixed_params(11);

  state(3) = state_temp(0);
  state(4) = state_temp(1);
  state(5) = state_temp(2);
  state(6) = state_temp(3);
  state(10) = state_temp(4);
  state(11) = state_temp(5);
  state(12) = state_temp(6);
  state(13) = state_temp(7);
  state(17) = state_temp(8);
  state(18) = state_temp(9);
  state(19) = state_temp(10);
  state(20) = state_temp(11);
  state(24) = state_temp(12);

  // Second phase of optimization (optional)
  // In this phase only w1, w2, w3 are optimizing

  // Fill in array of parameters we are not intented to optimized
  // We still need to pass this parameters to optimizer because we need to compute
  // estimated signal during optimization and it requireds full state
  fixed_params.resize(22);
  fixed_params(0) = state(0);
  fixed_params(1) = state(1);
  fixed_params(2) = state(2);
  fixed_params(3) = state(3);
  fixed_params(4) = state(4);
  fixed_params(5) = state(5);
  fixed_params(6) = state(6);
  fixed_params(7) = state(7);
  fixed_params(8) = state(8);
  fixed_params(9) = state(9);
  fixed_params(10) = state(10);
  fixed_params(11) = state(11);
  fixed_params(12) = state(12);
  fixed_params(13) = state(13);
  fixed_params(14) = state(14);
  fixed_params(15) = state(15);
  fixed_params(16) = state(16);
  fixed_params(17) = state(17);
  fixed_params(18) = state(18);
  fixed_params(19) = state(19);
  fixed_params(20) = state(20);
  fixed_params(21) = state(24);

  state_temp.resize(3);
  state_temp(0) = state(21);
  state_temp(1) = state(22);
  state_temp(2) = state(23);

  //std::cout << "before\n " << state_temp.transpose() << std::endl;

  ukfVectorType lowerBound2(3);
  ukfVectorType upperBound2(3);

  // Lower bound
  lowerBound2[0] = lowerBound2[1] = lowerBound2[2] = 0.0;

  // Upper bound
  upperBound2[0] = upperBound2[1] = upperBound2[2] = 1.0;

  // init solver with bounds
  _lbfgsb->setSignal(signal);
  _lbfgsb->setFixed(fixed_params);
  _lbfgsb->setLowerBound(lowerBound2);
  _lbfgsb->setUpperBound(upperBound2);
  _lbfgsb->setPhase(2);
  // Run solver
  _lbfgsb->Solve(state_temp);

  state_temp = _lbfgsb->XOpt;
  //cout << "after " << state_temp.transpose() << endl;

  // Fill back the state tensor to return it the callee
  state(0) = fixed_params(0);
  state(1) = fixed_params(1);
  state(2) = fixed_params(2);
  state(3) = fixed_params(3);
  state(4) = fixed_params(4);
  state(5) = fixed_params(5);
  state(6) = fixed_params(6);
  state(7) = fixed_params(7);
  state(8) = fixed_params(8);
  state(9) = fixed_params(9);
  state(10) = fixed_params(10);
  state(11) = fixed_params(11);
  state(12) = fixed_params(12);
  state(13) = fixed_params(13);
  state(14) = fixed_params(14);
  state(15) = fixed_params(15);
  state(16) = fixed_params(16);
  state(17) = fixed_params(17);
  state(18) = fixed_params(18);
  state(19) = fixed_params(19);
  state(20) = fixed_params(20);
  state(24) = fixed_params(21);

  state(21) = state_temp(0);
  state(22) = state_temp(1);
  state(23) = state_temp(2);

  //std::cout << "state after \n" << state << std::endl;
}

void Tractography::InverseStateDiffusionPropagator(stdVecState &reference, stdVecState &inverted)
{
  //assert(reference.size() == 25);
  //assert(inverted.size() == 25);

  for (unsigned int it = 0; it < reference.size(); ++it)
  {
    if (it <= 2)
      inverted[it] = -reference[it];
    else
      inverted[it] = reference[it];
  }
}

void Tractography::StateToMatrix(State &state, ukfMatrixType &matrix)
{
  //assert(state.size() > 0);

  matrix.resize(state.size(), 1);

  for (int it = 0; it < state.size(); ++it)
    matrix(it, 0) = state[it];
}

void Tractography::MatrixToState(ukfMatrixType &matrix, State &state)
{
  //assert(matrix.cols() == 1);
  //assert(matrix.rows() > 0);

  state.resize(matrix.rows());

  for (int it = 0; it < matrix.rows(); ++it)
    state[it] = matrix(it, 0);
}

// FIXME: not clear why gradientStrength and pulseSeparation are passed as arguments when
//        they are already class members.
void Tractography::createProtocol(const ukfVectorType &_b_values,
                                  ukfVectorType &l_gradientStrength, ukfVectorType &l_pulseSeparation)
{
  std::vector<double> Bunique, tmpG;
  ukfPrecisionType Bmax = 0;
  ukfPrecisionType tmp, Gmax, GAMMA;

  l_gradientStrength.resize(_b_values.size());
  l_pulseSeparation.resize(_b_values.size());

  // set maximum G = 40 mT/m
  Gmax = 0.04;
  GAMMA = 267598700;

  for (int i = 0; i < _b_values.size(); ++i)
  {
    int unique = 1;
    for (unsigned int j = 0; j < Bunique.size(); ++j)
    {
      if (_b_values[i] == Bunique[j])
      {
        unique = 0;
        break;
      }
    }
    if (unique == 1)
    {
      Bunique.push_back(_b_values[i]);
    }
    if (Bmax < _b_values[i])
    {
      Bmax = _b_values[i];
    }
  }

  tmp = cbrt(3 * Bmax * 1000000 / (2 * GAMMA * GAMMA * Gmax * Gmax));

  for (int i = 0; i < _b_values.size(); ++i)
  {
    l_pulseSeparation[i] = tmp;
  }

  for (unsigned int i = 0; i < Bunique.size(); ++i)
  {
    tmpG.push_back(std::sqrt(Bunique[i] / Bmax) * Gmax);
    // std::cout<< "\n tmpG:" << std::sqrt(Bunique[i]/Bmax) * Gmax;
  }

  for (unsigned int i = 0; i < Bunique.size(); ++i)
  {
    for (int j = 0; j < _b_values.size(); j++)
    {
      if (_b_values[j] == Bunique[i])
      {
        l_gradientStrength[j] = tmpG[i];
      }
    }
  }
}

void Tractography::UnpackTensor(const ukfVectorType &b, // b - bValues
                                const stdVec_t &u,      // u - directions
                                stdEigVec_t &s,         // s = signal values
                                stdEigVec_t &ret)       // starting params [i][j] : i - signal number; j - param
{
  // DEBUGGING
  // std::cout << "b's: ";
  // for (int i=0; i<b.size();++i) {
  //   std::cout << b[i] << ", ";
  // }
  //assert(ret.size() == s.size());

  // Build B matrix.
  const int signal_dim = _signal_data->GetSignalDimension();

  /**
  * A special type for holding 6 elements of tensor for each signal
  *  Only used in tractography.cc
  */
  typedef Eigen::Matrix<ukfPrecisionType, Eigen::Dynamic, 6> BMatrixType;
  BMatrixType B(signal_dim * 2, 6); //HACK: Eigen::Matrix<ukfPrecisionType, DYNAMIC, 6> ??

  for (int i = 0; i < signal_dim * 2; ++i)
  {
    const vec3_t &g = u[i];
    B(i, 0) = (-b[i]) * (g[0] * g[0]);
    B(i, 1) = (-b[i]) * (2.0 * g[0] * g[1]);
    B(i, 2) = (-b[i]) * (2.0 * g[0] * g[2]);
    B(i, 3) = (-b[i]) * (g[1] * g[1]);
    B(i, 4) = (-b[i]) * (2.0 * g[1] * g[2]);
    B(i, 5) = (-b[i]) * (g[2] * g[2]);
  }

  // Use QR decomposition to find the matrix representation of the tensor at the
  // seed point of the fiber. Raplacement of the gmm function gmm::least_squares_cg(..)
  Eigen::HouseholderQR<BMatrixType> qr(B);

  // Temporary variables.
  mat33_t D;

  if (this->debug)
    std::cout << "Estimating seed tensors:" << std::endl;

  // Unpack data
  for (stdEigVec_t::size_type i = 0; i < s.size(); ++i)
  {
    // We create a temporary vector to store the signal when the log function is applied
    ukfVectorType log_s;
    log_s.resize(s[i].size());

    for (unsigned int j = 0; j < s[i].size(); ++j)
    {
      if (s[i][j] <= 0)
        s[i][j] = 10e-8;

      log_s[j] = log(s[i][j]);
    }

    // The six tensor components.
    //TODO: this could be fixed size
    //ukfVectorType d = qr.solve(s[i]);
    ukfVectorType d = qr.solve(log_s);

    // symmetric diffusion tensor
    D(0, 0) = d[0];
    D(0, 1) = d[1];
    D(0, 2) = d[2];
    D(1, 0) = d[1];
    D(1, 1) = d[3];
    D(1, 2) = d[4];
    D(2, 0) = d[2];
    D(2, 1) = d[4];
    D(2, 2) = d[5];
    // Use singular value decomposition to extract the eigenvalues and the
    // rotation matrix (which gives us main direction of the tensor).
    // NOTE that svd can be used here only because D is a normal matrix

    // std::cout << "Tensor test: " << std::endl << D << std::endl;
    Eigen::JacobiSVD<ukfMatrixType> svd_decomp(D, Eigen::ComputeThinU);
    mat33_t Q = svd_decomp.matrixU();
    vec3_t sigma = svd_decomp.singularValues(); // diagonal() returns elements of a diag matrix as a vector.

    if (Q.determinant() < ukfZero)
      Q = Q * (-ukfOne);

    // Extract the three Euler Angles from the rotation matrix.
    ukfPrecisionType phi, psi;
    const ukfPrecisionType theta = std::acos(Q(2, 2));
    ukfPrecisionType epsilon = 1.0e-10;
    if (fabs(theta) > epsilon)
    {
      phi = atan2(Q(1, 2), Q(0, 2));
      psi = atan2(Q(2, 1), -Q(2, 0));
    }
    else
    {
      phi = atan2(-Q(0, 1), Q(1, 1));
      psi = ukfZero;
    }

    ret[i].resize(9);
    ret[i][0] = Q(0, 0);
    ret[i][1] = Q(1, 0);
    ret[i][2] = Q(2, 0);
    ret[i][3] = theta;
    ret[i][4] = phi;
    ret[i][5] = psi;
    sigma = sigma * GLOBAL_TENSOR_PACK_VALUE; // NOTICE this scaling of eigenvalues. The values are scaled back in diffusion_euler()
    ret[i][6] = sigma[0];
    ret[i][7] = sigma[1];
    ret[i][8] = sigma[2];
  }
}

void Tractography::Follow3T(const int thread_id,
                            const SeedPointInfo &fiberStartSeed,
                            UKFFiber &fiber,
                            unsigned char &is_discarded)
{
  // For ridgelets bi-exp model only!
  int fiber_size = 100;
  int fiber_length = 0;
  assert(_model->signal_dim() == _signal_data->GetSignalDimension() * 2);

  // Unpack the fiberStartSeed information.
  vec3_t x = fiberStartSeed.point;
  State state = fiberStartSeed.state;
  ukfMatrixType p(fiberStartSeed.covariance);
  /* 
  I don't create new variables in a SeedPointInfo for rtop values and keep
  everything in fa variables just to keep consistency with other models 
  and also, to avoid making new variables which are used only in Bi-exp model.
  I think think the idea of creating more new variables just for one model
  is a pure redundancy. 

  TODO: Rename this variables in case if only BiExp DP models remains and other deleted
  
  To simplify code 'readability' and understanding I make a local rtop variables
  in Follow3T and Step3T functions.
  */
  ukfPrecisionType rtop1 = fiberStartSeed.fa;
  ukfPrecisionType rtop2 = fiberStartSeed.fa2;
  ukfPrecisionType rtop3 = fiberStartSeed.fa3;
  ukfPrecisionType rtopModel = fiberStartSeed.trace;
  ukfPrecisionType rtopSignal = fiberStartSeed.trace2;
  ukfPrecisionType dNormMSE = 0; // no error at the fiberStartSeed

  ukfPrecisionType Fm1, lmd1, Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso;
  computeUncertaintiesCharacteristics(p, Fm1, lmd1, Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso);

  //std::cout << "For seed point \n " << fiberStartSeed.state << std::endl;

  //  Reserving fiber array memory so as to avoid resizing at every step
  FiberReserve(fiber, fiber_size);

  // Record start point.
  Record(x, rtop1, rtop2, rtop3, Fm1, lmd1, Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso, state, p, fiber, dNormMSE, rtopModel, rtopSignal);

  vec3_t m1 = fiberStartSeed.start_dir;
  vec3_t m2, m3;

  // Tract the fiber.
  ukfMatrixType signal_tmp(_model->signal_dim(), 1);
  ukfMatrixType state_tmp(_model->state_dim(), 1);

  int stepnr = 0;
  while (true)
  {
    ++stepnr;

    // That's one small step for a propagator, one giant leap for tractography...
    Step3T(thread_id, x, m1, m2, m3, state, p, dNormMSE, rtop1, rtop2, rtop3, Fm1, lmd1,
           Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso, rtopModel, rtopSignal);

    // Check if we should abort following this fiber. We abort if we reach the
    // CSF, if FA or GA get too small, if the curvature get's too high or if
    // the fiber gets too long.
    const bool is_brain = _signal_data->ScalarMaskValue(x) > 0; //_signal_data->Interp3ScalarMask(x) > 0.1;

    state_tmp.col(0) = state;

    _model->H(state_tmp, signal_tmp);

    //const ukfPrecisionType mean_signal = s2adc(signal_tmp);
    bool in_csf = false;
    if (_csf_provided)
      in_csf = _signal_data->ScalarCSFValue(x) > 0.5; // consider CSF as a true only if pve value > 0.5
    //else
    //in_csf = mean_signal < _mean_signal_min; // estimate 'CSF' which is basically GAF from a signal

    // ukfPrecisionType rtopSignal = trace2; // rtopSignal is stored in trace2

    //The trick is to discard fibers only when we have CSF mask?
    //Wonder why? Because we can't say if estimated from voxel 'CSF' is CSF.
    if (_csf_provided && in_csf)
    {
      is_discarded = 1; // mark fiber to remove it later
      break;
    }
    else
    {
      is_discarded = 0; // that's fiber is fine, we are going to keep it on
    }

    bool dNormMSE_too_high = dNormMSE > _max_nmse;
    bool is_curving = curve_radius(fiber.position) < _min_radius;
    bool in_rtop1 = rtop1 < _rtop1_min_stop;
    bool is_high_fw = state(24) > _fw_thresh;

    // Code for stop tracing with WM mask
    // if (_wm_provided)
    // {
    //  if (_signal_data->ScalarWMValue(x) < 0.30 || in_rtop1 || !is_brain || dNormMSE_too_high || stepnr > _max_length)
    //    break;
    //}
    //else
    //{

    if (!is_brain || in_rtop1 || is_high_fw || in_csf || is_curving || dNormMSE_too_high || stepnr > _max_length)
      break;
    //}

    if (fiber_length >= fiber_size)
    {
      // If fibersize is more than initially allocated size resizing further.
      fiber_size += 100;
      FiberReserve(fiber, fiber_size);
    }

    if ((stepnr + 1) % _steps_per_record == 0)
    {
      fiber_length++;
      Record(x, rtop1, rtop2, rtop3, Fm1, lmd1, Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso, state, p, fiber, dNormMSE, rtopModel, rtopSignal);
    }
  }
  FiberReserve(fiber, fiber_length);
}

void Tractography::Follow3T(const int thread_id,
                            const SeedPointInfo &fiberStartSeed,
                            UKFFiber &fiber,
                            UKFFiber &fiber1,
                            UKFFiber &fiber2,
                            UKFFiber &fiber3)
{
  // For ridgelets bi-exp model only!
  // Debugging version of bi-exp model. Provides functionality
  // to output every (out of 3) directions from the state vector
  int fiber_size = 100;
  int fiber_weight_size = fiber_size * 2 / 3;
  int fiber_length = 0;
  assert(_model->signal_dim() == _signal_data->GetSignalDimension() * 2);

  // Unpack the fiberStartSeed information.
  vec3_t x = fiberStartSeed.point;
  State state = fiberStartSeed.state;
  ukfMatrixType p(fiberStartSeed.covariance);
  /* 
  I don't create new variables in a SeedPointInfo for rtop values and keep
  everything in fa variables just to keep consistency with other models 
  and also, to avoid making new variables which are used only in Bi-exp model.
  I think think the idea of creating more new variables just for one model
  is a pure redundancy. 
  
  To simplify code 'reading' and understanding I make a local rtop variables
  in Follow3T and Step3T functions.
  */
  ukfPrecisionType rtop1 = fiberStartSeed.fa;
  ukfPrecisionType rtop2 = fiberStartSeed.fa2;
  ukfPrecisionType rtop3 = fiberStartSeed.fa3;
  ukfPrecisionType rtopModel = fiberStartSeed.trace;
  ukfPrecisionType rtopSignal = fiberStartSeed.trace2;
  ukfPrecisionType dNormMSE = 0; // no error at the fiberStartSeed

  ukfPrecisionType Fm1, lmd1, Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso;
  computeUncertaintiesCharacteristics(p, Fm1, lmd1, Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso);
  //std::cout << "For seed point \n " << fiberStartSeed.state << std::endl;

  // Reserving fiber array memory so as to avoid resizing at every step
  FiberReserve(fiber, fiber_size);
  FiberReserveWeightTrack(fiber1, fiber_weight_size);
  FiberReserveWeightTrack(fiber2, fiber_weight_size);
  FiberReserveWeightTrack(fiber3, fiber_weight_size);

  // Record start point
  Record(x, rtop1, rtop2, rtop3, Fm1, lmd1, Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso, state, p, fiber, dNormMSE, rtopModel, rtopSignal);
  RecordWeightTrack(x, fiber1, state(0), state(1), state(2));
  RecordWeightTrack(x, fiber2, state(7), state(8), state(9));
  RecordWeightTrack(x, fiber3, state(14), state(15), state(16));

  vec3_t m1 = fiberStartSeed.start_dir;
  vec3_t m2, m3;

  // Tract the fiber.
  ukfMatrixType signal_tmp(_model->signal_dim(), 1);
  ukfMatrixType state_tmp(_model->state_dim(), 1);

  int stepnr = 0;
  while (true)
  {
    // std::cout << "step " << stepnr << std::endl;
    ++stepnr;

    Step3T(thread_id, x, m1, m2, m3, state, p, dNormMSE, rtop1, rtop2, rtop3, Fm1, lmd1,
           Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso, rtopModel, rtopSignal);

    // cout << "w's " << state(21) << " " << state(22) << " " << state(23) << endl;
    // Check if we should abort following this fiber. We abort if we reach the
    // CSF, if FA or GA get too small, if the curvature get's too high or if
    // the fiber gets too long.
    const bool is_brain = _signal_data->ScalarMaskValue(x) > 0; //_signal_data->Interp3ScalarMask(x) > 0.1;

    state_tmp.col(0) = state;

    _model->H(state_tmp, signal_tmp);

    const ukfPrecisionType mean_signal = s2adc(signal_tmp);
    bool in_csf = (mean_signal < _mean_signal_min);

    // ukfPrecisionType rtopSignal = trace2; // rtopSignal is stored in trace2

    // in_csf = rtopSignal < _rtop_min;
    bool in_rtop1 = rtop1 < 4000;

    bool is_high_fw = state(24) > 0.7;
    bool in_rtop = rtopModel < 15000; // means 'in rtop' threshold
    bool dNormMSE_too_high = dNormMSE > _max_nmse;
    bool is_curving = curve_radius(fiber.position) < _min_radius;

    // stepnr > _max_length // Stop if the fiber is too long - Do we need this???
    if (!is_brain || in_rtop || in_rtop1 || is_high_fw || in_csf || is_curving || dNormMSE_too_high)
    {
      break;
    }

    if (fiber_length >= fiber_size)
    {
      // If fibersize is more than initally allocated size resizing further
      fiber_size += 100;
      FiberReserve(fiber, fiber_size);
      FiberReserveWeightTrack(fiber1, fiber_weight_size);
      FiberReserveWeightTrack(fiber2, fiber_weight_size);
      FiberReserveWeightTrack(fiber3, fiber_weight_size);
    }

    if ((stepnr + 1) % _steps_per_record == 0)
    {
      fiber_length++;
      Record(x, rtop1, rtop2, rtop3, Fm1, lmd1, Fm2, lmd2, Fm3, lmd3, varW1, varW2, varW3, varWiso, state, p, fiber, dNormMSE, rtopModel, rtopSignal);
      if ((stepnr + 1) % 3 == 0)
      {
        RecordWeightTrack(x, fiber1, state(0), state(1), state(2));
        RecordWeightTrack(x, fiber2, state(7), state(8), state(9));
        RecordWeightTrack(x, fiber3, state(14), state(15), state(16));
      }
    }
  }
  FiberReserve(fiber, fiber_length);
  FiberReserveWeightTrack(fiber1, fiber_weight_size);
  FiberReserveWeightTrack(fiber2, fiber_weight_size);
  FiberReserveWeightTrack(fiber3, fiber_weight_size);
}

void Tractography::Step3T(const int thread_id,
                          vec3_t &x,
                          vec3_t &m1,
                          vec3_t &m2,
                          vec3_t &m3,
                          State &state,
                          ukfMatrixType &covariance,
                          ukfPrecisionType &dNormMSE,
                          ukfPrecisionType &rtop1,
                          ukfPrecisionType &rtop2,
                          ukfPrecisionType &rtop3,
                          ukfPrecisionType &Fm1,
                          ukfPrecisionType &lmd1,
                          ukfPrecisionType &Fm2,
                          ukfPrecisionType &lmd2,
                          ukfPrecisionType &Fm3,
                          ukfPrecisionType &lmd3,
                          ukfPrecisionType &varW1,
                          ukfPrecisionType &varW2,
                          ukfPrecisionType &varW3,
                          ukfPrecisionType &varWiso,
                          ukfPrecisionType &rtopModel,
                          ukfPrecisionType &rtopSignal)
{
  // For ridgelets bi-exp model
  /*
  assert(static_cast<int>(covariance.cols()) == _model->state_dim() &&
         static_cast<int>(covariance.rows()) == _model->state_dim());
  assert(static_cast<int>(state.size()) == _model->state_dim());
  */
  State state_new(_model->state_dim());
  State state_prev(_model->state_dim());
  state_prev = state;

  ukfMatrixType covariance_new(_model->state_dim(), _model->state_dim());
  covariance_new.setConstant(ukfZero);

  // Use the Unscented Kalman Filter to get the next estimate.
  ukfVectorType signal(_signal_data->GetSignalDimension() * 2);
  _signal_data->Interp3Signal(x, signal);

  // Estimated state until acceptable convergence or number iterations exceeded
  LoopUKF(thread_id, state, covariance, signal, state_new, covariance_new, dNormMSE);

  vec3_t old_dir = m1;

  _model->State2Tensor3T(state, old_dir, m1, m2, m3);

  ukfPrecisionType _rtop1, _rtop2, _rtop3, _rtopModel, _rtopSignal, _Fm1, _lmd1, _Fm2, _lmd2, _Fm3, _lmd3, _varW1, _varW2, _varW3, _varWiso;

  computeRTOPfromState(state, _rtopModel, _rtop1, _rtop2, _rtop3);
  computeRTOPfromSignal(_rtopSignal, signal);
  computeUncertaintiesCharacteristics(covariance, _Fm1, _lmd1, _Fm2, _lmd2, _Fm3, _lmd3, _varW1, _varW2, _varW3, _varWiso);

  rtop1 = _rtop1;
  rtop2 = _rtop2;
  rtop3 = _rtop3;
  rtopModel = _rtopModel;
  rtopSignal = _rtopSignal;

  Fm1 = _Fm1;
  lmd1 = _lmd1;
  Fm2 = _Fm2;
  lmd2 = _lmd2;
  Fm3 = _Fm3;
  lmd3 = _lmd3;
  varW1 = _varW1;
  varW2 = _varW2;
  varW3 = _varW3;
  varWiso = _varWiso;

  vec3_t dx;
  {
    const vec3_t dir = m1; // The dir is a unit vector in ijk coordinate system indicating the direction of step
    const vec3_t voxel = _signal_data->voxel();
    dx << dir[2] / voxel[0], // By dividing by the voxel size, it's guaranteed that the step
        // represented by dx is 1mm in RAS coordinate system, no matter whether
        // the voxel is isotropic or not
        dir[1] / voxel[1], // The value is scaled back during the ijk->RAS transformation when
        // outputted
        dir[0] / voxel[2];

    x = x + dx * _stepLength; // The x here is in ijk coordinate system.
  }
}

void Tractography::Step3T(const int thread_id,
                          vec3_t &x,
                          vec3_t &m1,
                          vec3_t &l1,
                          vec3_t &m2,
                          vec3_t &l2,
                          vec3_t &m3,
                          vec3_t &l3,
                          ukfPrecisionType &fa,
                          ukfPrecisionType &fa2,
                          ukfPrecisionType &fa3,
                          State &state,
                          ukfMatrixType &covariance,
                          ukfPrecisionType &dNormMSE,
                          ukfPrecisionType &trace,
                          ukfPrecisionType &trace2)
{
  /*
  assert(static_cast<int>(covariance.cols()) == _model->state_dim() &&
         static_cast<int>(covariance.rows()) == _model->state_dim());
  assert(static_cast<int>(state.size()) == _model->state_dim());
  */
  State state_new(_model->state_dim());

  ukfMatrixType covariance_new(_model->state_dim(), _model->state_dim());

  // Use the Unscented Kalman Filter to get the next estimate.
  ukfVectorType signal(_signal_data->GetSignalDimension() * 2);
  _signal_data->Interp3Signal(x, signal);
  _ukf[thread_id]->Filter(state, covariance, signal, state_new, covariance_new, dNormMSE);

  state = state_new;
  covariance = covariance_new;

  vec3_t old_dir = m1;

  _model->State2Tensor3T(state, old_dir, m1, l1, m2, l2, m3, l3);
  trace = l1[0] + l1[1] + l1[2];
  trace2 = l2[0] + l2[1] + l2[2];

  ukfPrecisionType dot1 = m1.dot(old_dir);
  ukfPrecisionType dot2 = m2.dot(old_dir);
  ukfPrecisionType dot3 = m3.dot(old_dir);

  if (dot1 < dot2 && dot3 < dot2)
  {
    // Switch dirs and lambdas.
    vec3_t tmp = m1;
    m1 = m2;
    m2 = tmp;
    tmp = l1;
    l1 = l2;
    l2 = tmp;

    // Swap state.

    SwapState(state, covariance, 2);
  }
  else if (dot1 < dot3)
  {
    // Switch dirs and lambdas.
    vec3_t tmp = m1;
    m1 = m3;
    m3 = tmp;
    tmp = l1;
    l1 = l3;
    l3 = tmp;

    // Swap state.
    SwapState(state, covariance, 3);
  }

  // Update FA. If the first lamba is not the largest anymore the FA is set to
  // 0, and the 0 FA value will lead to abortion in the tractography loop.
  if (l1[0] < l1[1] || l1[0] < l1[2])
  {
    fa = ukfZero;
  }
  else
  {
    fa = l2fa(l1[0], l1[1], l1[2]);
    fa2 = l2fa(l2[0], l2[1], l2[2]);
    fa3 = l2fa(l3[0], l3[1], l3[2]);
  }

  const vec3_t &voxel = _signal_data->voxel();

  // CB: Bug corrected, dir[i] should be divided by voxel[i]
  vec3_t dx;
  dx << m1[2] / voxel[0],
      m1[1] / voxel[1],
      m1[0] / voxel[2];
  x = x + dx * _stepLength;
}

void Tractography::LoopUKF(const int thread_id,
                           State &state,
                           ukfMatrixType &covariance,
                           ukfVectorType &signal,
                           State &state_new,
                           ukfMatrixType &covariance_new,
                           ukfPrecisionType &dNormMSE)
{
  _ukf[thread_id]->Filter(state, covariance, signal, state_new, covariance_new, dNormMSE);

  state = state_new;
  covariance = covariance_new;

  ukfPrecisionType er_org = dNormMSE;
  ukfPrecisionType er = er_org;

  State state_prev = state;

  for (int jj = 0; jj < _maxUKFIterations; ++jj)
  {
    _ukf[thread_id]->Filter(state, covariance, signal, state_new, covariance_new, dNormMSE);
    state = state_new;

    er_org = er;
    er = dNormMSE;

    if (er_org - er < 0.001) // if error is fine then stop
      break;

    state_prev = state;
  }

  state = state_prev;
}

void Tractography::SwapState(stdVecState &state,
                             ukfMatrixType &covariance,
                             int i)
{
  State tmp_state = ConvertVector<stdVecState, State>(state);
  SwapState(tmp_state, covariance, i);
  state = ConvertVector<State, stdVecState>(tmp_state);
}

void Tractography::SwapState(State &state,
                             ukfMatrixType &covariance,
                             int i)
{
  // This function is only for Bi-exp model
  assert(i == 2 || i == 3);
  int ishift = i - 1;

  int state_dim = _model->state_dim();
  assert(state_dim == 25);

  ukfMatrixType tmp(state_dim, state_dim);
  state_dim = 7;
  --i;
  int j = i == 1 ? 2 : 1;
  i *= state_dim;
  j *= state_dim;

  int tshift = 3 * state_dim;
  int mshift = ishift * state_dim;

  tmp = covariance;
  covariance.block(i, i, state_dim, state_dim) = tmp.block(0, 0, state_dim, state_dim);
  covariance.block(0, 0, state_dim, state_dim) = tmp.block(i, i, state_dim, state_dim);

  covariance.block(0, i, state_dim, state_dim) = tmp.block(i, 0, state_dim, state_dim);
  covariance.block(i, 0, state_dim, state_dim) = tmp.block(0, i, state_dim, state_dim);

  covariance.block(j, i, state_dim, state_dim) = tmp.block(j, 0, state_dim, state_dim);
  covariance.block(j, 0, state_dim, state_dim) = tmp.block(j, i, state_dim, state_dim);

  covariance.block(i, j, state_dim, state_dim) = tmp.block(0, j, state_dim, state_dim);
  covariance.block(0, j, state_dim, state_dim) = tmp.block(i, j, state_dim, state_dim);

  // Swap weights in covariance matrix
  // Lower parp
  covariance.block(tshift, mshift, 4, state_dim) = tmp.block(tshift, 0, 4, state_dim);
  covariance.block(tshift, 0, 4, state_dim) = tmp.block(tshift, mshift, 4, state_dim);
  // Right part
  covariance.block(mshift, tshift, state_dim, 4) = tmp.block(0, tshift, state_dim, 4);
  covariance.block(0, tshift, state_dim, 4) = tmp.block(mshift, tshift, state_dim, 4);

  // Lower right 4x4 matrix
  // !!!Need to check this!!!!
  int corn_shift = tshift + ishift;
  covariance(corn_shift, corn_shift) = tmp(tshift, tshift);
  covariance(tshift, tshift) = tmp(corn_shift, corn_shift);

  covariance(tshift, corn_shift) = tmp(corn_shift, tshift);
  covariance(corn_shift, tshift) = tmp(tshift, corn_shift);

  int oneshift = tshift + 1;
  int twoshift = tshift + 2;

  if (ishift == 1)
  {
    covariance.block<2, 1>(twoshift, tshift) = tmp.block<2, 1>(twoshift, oneshift);
    covariance.block<2, 1>(twoshift, oneshift) = tmp.block<2, 1>(twoshift, tshift);

    covariance.block<1, 2>(tshift, twoshift) = tmp.block<1, 2>(oneshift, twoshift);
    covariance.block<1, 2>(oneshift, twoshift) = tmp.block<1, 2>(tshift, twoshift);
  }
  else if (ishift == 2)
  {
    // Horizontal mid
    covariance(oneshift, tshift) = tmp(oneshift, twoshift);
    covariance(oneshift, twoshift) = tmp(oneshift, tshift);

    // Vertical mid
    covariance(twoshift, oneshift) = tmp(tshift, oneshift);
    covariance(tshift, oneshift) = tmp(twoshift, oneshift);

    int threeshift = tshift + 3;
    // Horizontal bottom
    covariance(threeshift, twoshift) = tmp(threeshift, tshift);
    covariance(threeshift, tshift) = tmp(threeshift, twoshift);
    // Vertical right
    covariance(twoshift, threeshift) = tmp(tshift, threeshift);
    covariance(tshift, threeshift) = tmp(twoshift, threeshift);
  }
  else
  {
    std::cout << "Error: BiExp swap state function works only for 3 Tensors.\n";
    throw;
  }

  // Swap the state
  const ukfVectorType tmp_vec = state;
  state.segment(i, state_dim) = tmp_vec.segment(0, state_dim);
  state.segment(0, state_dim) = tmp_vec.segment(i, state_dim);

  const ukfPrecisionType tmp_weight = state(21);
  int iw = 21 + ishift;
  state(21) = state(iw);
  state(iw) = tmp_weight;
}

void Tractography::Record(const vec3_t &x, const ukfPrecisionType fa, const ukfPrecisionType fa2, const ukfPrecisionType fa3, const ukfPrecisionType Fm1,
                          const ukfPrecisionType lmd1, const ukfPrecisionType Fm2, const ukfPrecisionType lmd2, const ukfPrecisionType Fm3,
                          const ukfPrecisionType lmd3, const ukfPrecisionType varW1, const ukfPrecisionType varW2, const ukfPrecisionType varW3,
                          const ukfPrecisionType varWiso, const State &state, const ukfMatrixType p, UKFFiber &fiber, const ukfPrecisionType dNormMSE,
                          const ukfPrecisionType trace, const ukfPrecisionType trace2)
{
  /*
  // if Noddi model is used Kappa is stored in trace, Vic in fa and Viso in freewater
  assert(_model->state_dim() == static_cast<int>(state.size()));
  assert(p.rows() == static_cast<unsigned int>(state.size()) &&
         p.cols() == static_cast<unsigned int>(state.size()));
*/
  // std::cout << "x: " << x[0] << " " << x[1] << " " << x[2] << std::endl;
  fiber.position.push_back(x);
  fiber.norm.push_back(p.norm());

  if (_record_nmse)
  {
    fiber.normMSE.push_back(dNormMSE);
  }

  if (_record_rtop)
  {
    fiber.trace.push_back(trace);
    fiber.trace2.push_back(trace2);
  }

  if (_record_rtop)
  {
    fiber.fa.push_back(fa);
    if (_num_tensors >= 2)
    {
      fiber.fa2.push_back(fa2);
    }
    if (_num_tensors == 3)
    {
      fiber.fa3.push_back(fa3);
    }
  }

  if (_record_weights)
  {
    ukfPrecisionType w1 = state[21];
    ukfPrecisionType w2 = state[22];
    ukfPrecisionType w3 = state[23];
    ukfPrecisionType wiso = state[24];

    fiber.w1.push_back(w1);
    fiber.w2.push_back(w2);
    fiber.w3.push_back(w3);
    fiber.free_water.push_back(wiso);

    /* Angles */
    State store_state(state);
    vec3_t dir1;
    initNormalized(dir1, store_state[0], store_state[1], store_state[2]);
    vec3_t dir2;
    initNormalized(dir2, store_state[7], store_state[8], store_state[9]);
    vec3_t dir3;
    initNormalized(dir3, store_state[14], store_state[15], store_state[16]);

    ukfPrecisionType d1d2 = std::min(RadToDeg(std::acos(dir1.dot(dir2))), RadToDeg(std::acos(dir1.dot(-dir2))));
    ukfPrecisionType d1d3 = std::min(RadToDeg(std::acos(dir1.dot(dir3))), RadToDeg(std::acos(dir1.dot(-dir3))));

    fiber.w1w2angle.push_back(d1d2);
    fiber.w1w3angle.push_back(d1d3);
  }

  if (_record_free_water)
  {
    ukfPrecisionType fw = 1 - state[_nPosFreeWater];
    // sometimes QP produces slightly negative results due to numerical errors in Quadratic Programming, the weight is
    // clipped in F() and H() but its still possible that
    // a slightly negative weight gets here, because the filter ends with a constrain step.
    if (fw < 0)
    {
      if (fw >= -1.0e-4) // for small errors just round it to 0
      {
        fw = 0;
      }
      else // for too big errors exit with exception.
      {
        std::cout << "Error: program produced negative free water.\n";
        throw;
      }
    }
    fiber.free_water.push_back(fw);
  }

  // Record the state
  State store_state(state);
  vec3_t dir;

  // normalize m1
  initNormalized(dir, store_state[0], store_state[1], store_state[2]);
  store_state[0] = dir[0];
  store_state[1] = dir[1];
  store_state[2] = dir[2];

  // normalize m2
  initNormalized(dir, store_state[7], store_state[8], store_state[9]);
  store_state[7] = dir[0];
  store_state[8] = dir[1];
  store_state[9] = dir[2];

  // normalize m2
  initNormalized(dir, store_state[14], store_state[15], store_state[16]);
  store_state[14] = dir[0];
  store_state[15] = dir[1];
  store_state[16] = dir[2];

  fiber.state.push_back(store_state);

  if (_record_uncertainties)
  {
    fiber.Fm1.push_back(Fm1);
    fiber.lmd1.push_back(lmd1);
    fiber.Fm2.push_back(Fm2);
    fiber.lmd2.push_back(lmd2);
    fiber.Fm3.push_back(Fm3);
    fiber.lmd3.push_back(lmd3);
    fiber.varW1.push_back(varW1);
    fiber.varW2.push_back(varW2);
    fiber.varW3.push_back(varW3);
    fiber.varWiso.push_back(varWiso);
  }

  if (_record_cov)
  {
    fiber.covariance.push_back(p);
  }
}

void Tractography::Record(const vec3_t &x, const ukfPrecisionType fa, const ukfPrecisionType fa2, const ukfPrecisionType fa3, const State &state,
                          const ukfMatrixType p, UKFFiber &fiber, const ukfPrecisionType dNormMSE, const ukfPrecisionType trace, const ukfPrecisionType trace2)
{
  // if Noddi model is used Kappa is stored in trace, Vic in fa and Viso in freewater
  /*
  assert(_model->state_dim() == static_cast<int>(state.size()));
  assert(p.rows() == static_cast<unsigned int>(state.size()) &&
         p.cols() == static_cast<unsigned int>(state.size()));
*/
  // std::cout << "x: " << x[0] << " " << x[1] << " " << x[2] << std::endl;
  fiber.position.push_back(x);
  fiber.norm.push_back(p.norm());

  if (_record_nmse)
  {
    fiber.normMSE.push_back(dNormMSE);
  }

  if (_record_rtop)
  {
    fiber.trace.push_back(trace);
    fiber.trace2.push_back(trace2);
  }

  if (_record_rtop)
  {
    fiber.fa.push_back(fa);
    fiber.fa2.push_back(fa2);
    fiber.fa3.push_back(fa3);
  }

  if (_record_weights)
  {
    ukfPrecisionType w1 = state[21];
    ukfPrecisionType w2 = state[22];
    ukfPrecisionType w3 = state[23];
    ukfPrecisionType wiso = state[24];

    fiber.w1.push_back(w1);
    fiber.w2.push_back(w2);
    fiber.w3.push_back(w3);
    fiber.free_water.push_back(wiso);

    /* Angles */
    State store_state(state);
    vec3_t dir1;
    initNormalized(dir1, store_state[0], store_state[1], store_state[2]);
    vec3_t dir2;
    initNormalized(dir2, store_state[7], store_state[8], store_state[9]);
    vec3_t dir3;
    initNormalized(dir3, store_state[14], store_state[15], store_state[16]);

    ukfPrecisionType d1d2 = std::min(RadToDeg(std::acos(dir1.dot(dir2))), RadToDeg(std::acos(dir1.dot(-dir2))));
    ukfPrecisionType d1d3 = std::min(RadToDeg(std::acos(dir1.dot(dir3))), RadToDeg(std::acos(dir1.dot(-dir3))));

    fiber.w1w2angle.push_back(d1d2);
    fiber.w1w3angle.push_back(d1d3);
  }

  if (_record_free_water)
  {
    ukfPrecisionType fw = 1 - state[_nPosFreeWater];
    // sometimes QP produces slightly negative results due to numerical errors in Quadratic Programming, the weight is
    // clipped in F() and H() but its still possible that
    // a slightly negative weight gets here, because the filter ends with a constrain step.
    if (fw < 0)
    {
      if (fw >= -1.0e-4) // for small errors just round it to 0
      {
        fw = 0;
      }
      else // for too big errors exit with exception.
      {
        std::cout << "Error: program produced negative free water.\n";
        throw;
      }
    }
    fiber.free_water.push_back(fw);
  }

  // Record the state
  State store_state(state);
  vec3_t dir;

  // normalize m1
  initNormalized(dir, store_state[0], store_state[1], store_state[2]);
  store_state[0] = dir[0];
  store_state[1] = dir[1];
  store_state[2] = dir[2];

  // normalize m2
  initNormalized(dir, store_state[7], store_state[8], store_state[9]);
  store_state[7] = dir[0];
  store_state[8] = dir[1];
  store_state[9] = dir[2];

  // normalize m2
  initNormalized(dir, store_state[14], store_state[15], store_state[16]);
  store_state[14] = dir[0];
  store_state[15] = dir[1];
  store_state[16] = dir[2];

  fiber.state.push_back(store_state);

  if (_record_cov)
  {
    fiber.covariance.push_back(p);
  }
}

void Tractography::RecordWeightTrack(const vec3_t &x, UKFFiber &fiber, ukfPrecisionType d1, ukfPrecisionType d2, ukfPrecisionType d3)
{
  vec3_t x1;
  vec3_t x2;

  vec3_t dx;
  {
    const vec3_t voxel = _signal_data->voxel();
    dx << d3 / voxel[0],
        d2 / voxel[1],
        d1 / voxel[2];

    x1 = x - dx * _stepLength;
    x2 = x + dx * _stepLength;
  }
  fiber.position.push_back(x1);
  fiber.position.push_back(x2);
}

void Tractography::FiberReserve(UKFFiber &fiber, int fiber_size)
{
  // Reserving space for fiber
  fiber.position.reserve(fiber_size);
  fiber.norm.reserve(fiber_size);
  fiber.state.reserve(fiber_size);
  if (_record_nmse)
  {
    fiber.normMSE.reserve(fiber_size);
  }

  if (_record_rtop)
  {
    fiber.fa.reserve(fiber_size);
    if (_num_tensors >= 2)
    {
      fiber.fa2.reserve(fiber_size);
    }
    if (_num_tensors >= 3)
    {
      fiber.fa3.reserve(fiber_size);
    }
  }
  if (_record_free_water)
  {
    fiber.free_water.reserve(fiber_size);
  }
  if (_record_weights)
  {
    fiber.w1.reserve(fiber_size);
    fiber.w2.reserve(fiber_size);
    fiber.w3.reserve(fiber_size);
    fiber.free_water.reserve(fiber_size);
    fiber.w1w2angle.reserve(fiber_size);
    fiber.w1w3angle.reserve(fiber_size);
  }
  if (_record_cov)
  {
    fiber.covariance.reserve(fiber_size);
  }
}

void Tractography::FiberReserveWeightTrack(UKFFiber &fiber, int fiber_size)
{
  // Reserving space for fiber
  fiber.position.reserve(fiber_size);
}
