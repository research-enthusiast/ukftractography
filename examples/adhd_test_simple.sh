start=`date +%s`

SRC="../UKFTractography"

#logFile Name
logFile="log.txt"

# BINARY
BINARY='../build/UKFTractography-build/UKFTractography/bin/UKFTractography'

# VOLUME
dwi_path="/home/rinat/Desktop/ukftests/dwi-Aligned-Ed-Bet-Merged-unring-Epi.nhdr"

# MASK
mask_path="/home/rinat/Desktop/ukftests/epi_corrected_tensormask.nrrd"

# SEEDS
seeds_path="/home/rinat/Desktop/ukftests/Segmentation-label.nrrd"

# OUTPUT FIBER
output_path='/home/rinat/Desktop/ukftests/seeds_tc.vtk'

# original numT = 2, --diffusionPropagator, --minRTOP 20
eval $BINARY \
 --dwiFile $dwi_path \
 --maskFile $mask_path \
 --tracts $output_path \
 --seedsFile $seeds_path \
 --seedsPerVoxel 1 \
 --numTensor 2 | tee $logFile
 end=`date +%s`

runtime=$((end - start))
echo "Output file name $output_path" | tee -a $logFile
echo "CPU Runtime was $runtime sec."  | tee -a $logFile

#diff $output_path $SRC/ukf/Data/Baseline/seeds_tc.vtk