/**
 * \file UKFTractography.cxx
 * \brief Main file of the project
 *
 * In this file the input arguments are parsed and passed on to the tractography
 * Also the model choice happens here
*/

#include <cassert>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>

#include "cli.h"
#include "tractography.h"
#include "BRAINSThreadControl.h"

#include "vtkNew.h"
#include "vtkPolyData.h"

extern "C"
{
  int ModuleEntryPoint(int argc, char **argv)
  {
    UKFSettings ukf_settings;

    if (int stat = ukf_parse_cli(argc, argv, ukf_settings) != EXIT_SUCCESS)
      return stat;

    // NOTE:  When used as share libary one must be careful not to permanently reset number of threads
    //        for entire program (i.e. when used as a slicer modules.
    //        This also addresses the issue when the program is run as part of a batch processing
    //        system so that the number of cores allocated by scheduler is respected rather than
    //        blindly using all the cores that are found.
    //        This implementation is taken from extensive testing of the BRAINSTools
    // (this object will be deleted by RAII and return to original thread count)
    const BRAINSUtils::StackPushITKDefaultNumberOfThreads TempDefaultNumberOfThreadsHolder(ukf_settings.num_threads);
#if ITK_VERSION_MAJOR >= 5
    const int actualNumThreadsUsed = itk::MultiThreaderBase::GetGlobalDefaultNumberOfThreads();
#else
    const int actualNumThreadsUsed = itk::MultiThreader::GetGlobalDefaultNumberOfThreads();
#endif
    ukf_settings.num_threads = actualNumThreadsUsed;
    {
      std::cout << "Found " << actualNumThreadsUsed << " cores on your system." << std::endl;
      std::cout << "Running tractography with " << actualNumThreadsUsed << " thread(s)." << std::endl;
    }

    // TODO these have always been hard-coded here. But why?
    bool normalizedDWIData = false;
    bool outputNormalizedDWIData = false;

    // initializing super object
    std::cout << "min rtop debug 1" << ukf_settings.rtop_min << std::endl;
    Tractography *tract = new Tractography(ukf_settings);
    std::cout << "min rtop debug 2" << ukf_settings.rtop_min << std::endl;

    // if specified on command line, write out binary tract file
    tract->SetWriteBinary(!ukf_settings.writeAsciiTracts);
    tract->SetWriteCompressed(!ukf_settings.writeUncompressedTracts);

    int writeStatus = 0;
    try
    {
      if (tract->LoadFiles(ukf_settings.dwiFile,
                           ukf_settings.seedsFile,
                           ukf_settings.maskFile,
                           normalizedDWIData, outputNormalizedDWIData) == EXIT_FAILURE)
      {
        itkGenericExceptionMacro(<< "::LoadFiles failed with unknown error.");
      }
      std::cout << "min rtop debug 3" << ukf_settings.rtop_min << std::endl;
      tract->UpdateFilterModelType();
      std::cout << "min rtop debug 4" << ukf_settings.rtop_min << std::endl;

      // Run the tractography.
      writeStatus = tract->Run();
      std::cout << "min rtop debug 5" << ukf_settings.rtop_min << std::endl;
    }
    catch (itk::ExceptionObject &err)
    {
      std::cerr << "UKFTractography: ITK ExceptionObject caught!" << std::endl;
      std::cerr << err << std::endl;

      writeStatus = EXIT_FAILURE;
    }
    catch (std::exception &exc)
    {
      std::cerr << "UKFTractography: std::exception caught:" << std::endl;
      std::cerr << exc.what() << std::endl;

      writeStatus = EXIT_FAILURE;
    }
    catch (...)
    {
      std::cerr << "UKFTractography: Unknown exception caught!" << std::endl;

      writeStatus = EXIT_FAILURE;
    }

    // Clean up.
    delete tract;

    return writeStatus;
  }

}; // extern "C"
