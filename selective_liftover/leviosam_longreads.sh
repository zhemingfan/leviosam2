# LevioSAM pipeline to lift and re-align long reads
#
# Authors: Nae-Chyun Chen
#
# Distributed under the MIT license
# https://github.com/alshai/levioSAM
#
set -xp

ALN_RG=""
THR=$(nproc)
LEVIOSAM=leviosam
TIME=time # GNU time
MEASURE_TIME=1 # Set to a >0 value to measure time for each step
DEFER_DEST_BED=""
COMMIT_SOURCE_BED=""
ALLOWED_GAPS=10
ALN=minimap2


while getopts a:C:D:g:F:i:L:M:o:r:R:t:T: flag
do
    case "${flag}" in
        a) ALN=${OPTARG};;
        C) CLFT=${OPTARG};;
        D) DEFER_DEST_BED=" -D ${OPTARG}";;
        R) COMMIT_SOURCE_BED=" -r ${OPTARG}";;
        F) REF=${OPTARG};;
        g) ALLOWED_GAPS=${OPTARG};;
        i) INPUT=${OPTARG};;
        L) LEVIOSAM=${OPTARG};;
        M) MEASURE_TIME=${OPTARG};;
        o) PFX=${OPTARG};;
        r) ALN_RG=${OPTARG};;
        t) THR=${OPTARG};;
        T) TIME=${OPTARG};;
    esac
done

echo "Input BAM: ${INPUT}";
echo "Output prefix: ${PFX}";
echo "Aligner: ${ALN}";
echo "Reference: ${REF}";
echo "Aligner read group: ${ALN_RG}";
echo "LevioSAM software: ${LEVIOSAM}";
echo "LevioSAM index: ${CLFT}";
echo "Allowed gaps: ${ALLOWED_GAPS}";
echo "BED where reads get deferred: ${DEFER_DEST_BED}";
echo "BED where reads get discarded: ${COMMIT_SOURCE_BED}";
echo "Num. threads: ${THR}";

if [[ ! ${ALN} =~ ^(minimap2|winnowmap2)$ ]]; then
    echo "Invalid ${ALN}. Accepted input: minimap2, winnowmap2"
    exit
fi

TT=""
if (( ${MEASURE_TIME} > 0 )); then
    TT="${TIME} -v -ao leviosam.time_log "
fi

# Lifting over using leviosam
if [ ! -s ${PFX}-committed.bam ]; then
    ${TT} ${LEVIOSAM} lift -C ${CLFT} -a ${INPUT} -t ${THR} -p ${PFX} -O bam \
    -S lifted -G ${ALLOWED_GAPS} \
    ${DEFER_DEST_BED} ${COMMIT_SOURCE_BED}
fi

# Convert deferred reads to FASTQ
if [ ! -s ${PFX}-deferred.fq.gz ]; then
    ${TT} samtools fastq ${PFX}-deferred.bam | \
    ${TT} bgzip > ${PFX}-deferred.fq.gz
fi

# Re-align deferred reads
if [ ! -s ${PFX}-realigned.bam ]; then
    ${TT} ${ALN} -ax map-hifi -t ${THR} ${REF} ${PFX}-deferred.fq.gz | \
    ${TT} samtools view -hbo ${PFX}-realigned.bam
fi

# Merge and sort
if [ ! -s ${PFX}-final.bam ]; then
    ${TT} samtools cat ${PFX}-committed.bam ${PFX}-realigned.bam | \
    ${TT} samtools sort -@ ${THR} -o ${PFX}-final.bam
fi

