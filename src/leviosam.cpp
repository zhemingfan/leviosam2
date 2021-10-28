/*
 * liftover.cpp
 *
 * Classes and routines for translating (lifting over) coordinates between
 * two aligned sequences
 *
 * Authors: Taher Mun, Nae-Chyun Chen, Ben Langmead
 * Dept. of Computer Science, Johns Hopkins University
 *
 * Distributed under the MIT license
 * https://github.com/alshai/levioSAM
 */
#include <stdio.h>
#include <vector>
#include <zlib.h>
#include <getopt.h>
#include <htslib/kseq.h>
#include <htslib/kstring.h>
#include "leviosam.hpp"

KSEQ_INIT(gzFile, gzread)
;;


NameMap parse_name_map(const char* fname) {
    NameMap names;
    FILE* fp = fopen(fname, "r");
    char n1[255];
    char n2[255];
    int x;
    while ((x = fscanf(fp,"%[^\t]\t%[^\n]\n", n1, n2)) != EOF) {
        if (x <= 0) {
            fprintf(stderr, "error encountered reading name map\n");
            exit(1);
        }
        names.push_back(std::make_pair(std::string(n1), std::string(n2)));
    }
    fclose(fp);
    return names;
}

LengthMap parse_length_map(const char* fname) {
    LengthMap lengths;
    FILE* fp = fopen(fname, "r");
    int x;
    char n[255];
    char l[255];
    while ((x = fscanf(fp, "%[^\t]\t%[^\n]\n", n, l)) != EOF) {
        if (x <= 0 || lengths.find(n) != lengths.end()) {
            fprintf(stderr, "error encountered reading length map\n");
            exit(1);
        }
        lengths[n] = std::atoi(l);
    }
    fclose(fp);
    return lengths;
}


void serialize_run(lift_opts args) {
    if (args.outpre == "") {
        fprintf(stderr, "no output prefix specified! Use -p \n");
        print_serialize_help_msg();
        exit(1);
    }
    if (args.verbose) std::cerr << "verbose\n";
    if (args.vcf_fname != "") {
        lift::LiftMap l(lift::lift_from_vcf(
            args.vcf_fname, args.sample, args.haplotype,
            args.name_map, args.length_map));
        std::ofstream o(args.outpre + ".lft", std::ios::binary);
        l.serialize(o);
        fprintf(stderr, "levioSAM VcfMap saved to %s\n", (args.outpre + ".lft").data());
    } else if (args.chain_fname != "") {
        chain::ChainMap cfp(args.chain_fname, args.verbose, args.allowed_cigar_changes);
        std::ofstream o(args.outpre + ".clft", std::ios::binary);
        cfp.serialize(o);
        fprintf(stderr, "levioSAM ChainMap saved to %s\n", (args.outpre + ".clft").data());
    } else {
        fprintf(stderr, "Cannot build a levioSAM index. Neither -v or -c is properly set.\n");
        exit(1);
    }
}


/* Returns true if to an alignment can be committed
 */
bool commit_alignment(
    const bam1_t* const aln,
    const LevioSamUtils::WriteToFastq* const wfq
){
    const bam1_core_t* c = &(aln->core);
    std::vector<std::string> split_modes = LevioSamUtils::split_str(wfq->split_mode, ",");
    if (find(split_modes.begin(), split_modes.end(), "mapq") != split_modes.end()){
        if (c->qual < wfq->min_mapq)
            return false;
    }
    if (find(split_modes.begin(), split_modes.end(), "isize") != split_modes.end()){
        if (c->isize == 0 || c->isize > wfq->max_isize || c->isize < -wfq->max_isize)
            return false;
    }
    if (find(split_modes.begin(), split_modes.end(), "clipped_frac") != split_modes.end()){
        auto rlen = bam_cigar2rlen(c->n_cigar, bam_get_cigar(aln));
        if (1 - (rlen / c->l_qseq) > wfq->max_clipped_frac)
            return false;
    }
    if (find(split_modes.begin(), split_modes.end(), "aln_score") != split_modes.end()){
        if (bam_aux2i(bam_aux_get(aln, "AS")) < wfq->min_aln_score)
            return false;
    }
    return true;
}


template <class T>
void read_and_lift(
    T* lift_map,
    std::mutex* mutex_fread,
    std::mutex* mutex_fwrite,
    samFile* sam_fp,
    samFile* out_sam_fp,
    bam_hdr_t* hdr,
    int chunk_size,
    const std::map<std::string, std::string> &ref_dict,
    int md_flag,
    LevioSamUtils::WriteToFastq* wfq
){
    std::string current_contig;
    std::vector<bam1_t*> aln_vec;
    for (int i = 0; i < chunk_size; i++){
        bam1_t* aln = bam_init1();
        aln_vec.push_back(aln);
    }
    int read = 1;
    std::string ref;
    while (read >= 0){
        int num_actual_reads = chunk_size;
        {
            // read from SAM, protected by mutex
            std::lock_guard<std::mutex> g(*mutex_fread);
            for (int i = 0; i < chunk_size; i++){
                read = sam_read1(sam_fp, hdr, aln_vec[i]);
                if (read < 0){
                    num_actual_reads = i;
                    break;
                }
            }
        }
        for (int i = 0; i < num_actual_reads; i++){
            // TODO: Plan to remove `null_dest_contig`.
            // Need to test scenarios with `NameMap`
            std::string null_dest_contig;
            lift_map->lift_aln(aln_vec[i], hdr, null_dest_contig);
            if (md_flag) {
                auto tid = aln_vec[i]->core.tid;
                if (tid == -1) {
                    LevioSamUtils::remove_mn_md_tag(aln_vec[i]);
                } else {
                    std::string dest_contig(hdr->target_name[aln_vec[i]->core.tid]);
                    // change ref if needed
                    if (dest_contig != current_contig) {
                        ref = ref_dict.at(dest_contig);
                        current_contig = dest_contig;
                    }
                    bam_fillmd1(aln_vec[i], ref.data(), md_flag, 1);
                }
            }
            else { // strip MD and NM tags if md_flag not set bc the liftover invalidates them
                LevioSamUtils::remove_mn_md_tag(aln_vec[i]);
            }
        }
        for (int i = 0; i < num_actual_reads; i++) {
            // If a read is committed
            if (commit_alignment(aln_vec[i], wfq) == true) {
                // write to file, thread corruption protected by lock_guard
                std::lock_guard<std::mutex> g_commited(*mutex_fwrite);
                if (sam_write1(out_sam_fp, hdr, aln_vec[i]) < 0) {
                    std::cerr << "[Error] Failed to write record " << 
                        bam_get_qname(aln_vec[i]) << "\n";
                    exit(1);
                }
            } else {
                std::lock_guard<std::mutex> g_deferred(wfq->mutex_fwrite_fq);
                wfq->write_low_mapq_bam(aln_vec[i], hdr);
            }
        }
    }
    for (int i = 0; i < chunk_size; i++){
        bam_destroy1(aln_vec[i]);
    }
    aln_vec.clear();
}


std::map<std::string, std::string> load_fasta(std::string ref_name) {
    std::cerr << "Loading FASTA...";
    std::map<std::string, std::string> fmap;
    gzFile fp = gzopen(ref_name.data(), "r");
    kseq_t *seq;
    seq = kseq_init(fp);
    while (kseq_read(seq) >= 0) {
        fmap[std::string(seq->name.s)] = std::string(seq->seq.s);
    }
    kseq_destroy(seq);
    gzclose(fp);
    std::cerr << "done\n";
    return fmap;
}


void lift_run(lift_opts args) {
    std::cerr << "Loading levioSAM index...";
    chain::ChainMap chain_map = [&] {
        if (args.chainmap_fname != "") {
            std::ifstream in(args.chainmap_fname, std::ios::binary);
            return chain::ChainMap(
                in, args.verbose, args.allowed_cigar_changes
            );
        } else if (args.chain_fname != ""){
            return chain::ChainMap(
                args.chain_fname, args.verbose, args.allowed_cigar_changes
            );
        } else {
            return chain::ChainMap();
        }
    }();
    lift::LiftMap lift_map = [&]{
        if (args.lift_fname != "") {
            std::ifstream in(args.lift_fname, std::ios::binary);
            return lift::LiftMap(in);
        // if "-l" not specified, then create a levioSAM
        } else if (args.vcf_fname != "") {
            return lift::LiftMap(
                lift::lift_from_vcf(
                    args.vcf_fname, args.sample, args.haplotype,
                    args.name_map, args.length_map)
            );
        } else if ((args.chain_fname != "") || (args.chainmap_fname != "")) {
            return lift::LiftMap();
        } else {
            fprintf(stderr, "Not enough parameters specified to build/load lift-over\n");
            print_lift_help_msg();
            exit(1);
        }
    } ();

    std::cerr << "done\n";

    samFile* sam_fp = (args.sam_fname == "")?
        sam_open("-", "r") : sam_open(args.sam_fname.data(), "r");
    std::string out_mode = (args.out_format == "sam")? "w" : "wb";
    samFile* out_sam_fp = (args.outpre == "-" || args.outpre == "")?
        sam_open("-", out_mode.data()) :
        (args.split_mode == "")? // if split, append "-committed" after `outpre`
            sam_open((args.outpre + "." + args.out_format).data(), out_mode.data()) :
            sam_open((args.outpre + "-committed." + args.out_format).data(), out_mode.data());

    bam_hdr_t* hdr;
    if (args.chain_fname == "" || args.chainmap_fname == "") {
        hdr = chain_map.bam_hdr_from_chainmap(sam_fp);
    } else if (args.lift_fname != "" || args.vcf_fname != "") {
        hdr = lift_map.bam_hdr_from_liftmap(sam_fp);
    } else {
        fprintf(stderr, "Not enough parameters specified to build/load lift-over\n");
        print_lift_help_msg();
        exit(1);
    }
    sam_hdr_add_pg(hdr, "leviosam", "VN", VERSION, "CL", args.cmd.data(), NULL);
    auto write_hdr = sam_hdr_write(out_sam_fp, hdr);

    std::map<std::string, std::string> ref_dict;
    if (args.md_flag) {
        // load
        if (args.ref_name == "") {
            std::cerr << "Error: -m/--md -f <fasta> to be provided as well\n";
            exit(1);
        }
        ref_dict = load_fasta(args.ref_name);
    }

    LevioSamUtils::WriteToFastq wfq;
    if (args.split_mode != "") {
        std::vector<std::string> split_modes = LevioSamUtils::split_str(args.split_mode, ",");
        std::cerr << "Alignments don't pass the below filter are deferred:\n";
        if (find(split_modes.begin(), split_modes.end(), "mapq") != split_modes.end()){
            std::cerr << " - MAPQ (pre-liftover) < " << args.min_mapq << "\n";
        }
        if (find(split_modes.begin(), split_modes.end(), "isize") != split_modes.end()){
            std::cerr << " - TLEN/isize (post-liftover) > " << args.max_isize << "\n";
            std::cerr << " - TLEN/isize (post-liftover) == 0\n";
        }
        if (find(split_modes.begin(), split_modes.end(), "clipped_frac") != split_modes.end()){
            std::cerr << " - Fraction of clipped bases (post-liftover) < " << args.max_clipped_frac << "\n";
        }
        if (find(split_modes.begin(), split_modes.end(), "aln_score") != split_modes.end()){
            std::cerr << " - AS:i (pre-liftover) < " << args.min_aln_score << "\n";
        }
        wfq.init(
            args.outpre, args.split_mode,
            args.min_mapq, args.max_isize,
            args.max_clipped_frac, args.min_aln_score,
            args.out_format);
        auto write_hdr = sam_hdr_write(wfq.out_fp, hdr);
    }

    // const int num_threads = args.threads;
    // const int chunk_size = args.chunk_size;
    std::vector<std::thread> threads;
    std::mutex mutex_fread, mutex_fwrite;
    for (int j = 0; j < args.threads; j++){
        if (args.chain_fname == "" && args.chainmap_fname == "") {
            threads.push_back(
                std::thread(
                    read_and_lift<lift::LiftMap>,
                    &lift_map,
                    &mutex_fread, &mutex_fwrite,
                    sam_fp, out_sam_fp, hdr, args.chunk_size,
                    ref_dict, args.md_flag, &wfq));
        } else {
            threads.push_back(
                std::thread(
                    read_and_lift<chain::ChainMap>,
                    &chain_map,
                    &mutex_fread, &mutex_fwrite,
                    sam_fp, out_sam_fp, hdr, args.chunk_size,
                    ref_dict, args.md_flag, &wfq));
        }
    }
    for (int j = 0; j < args.threads; j++){
        if(threads[j].joinable())
            threads[j].join();
    }
    threads.clear();
    sam_close(sam_fp);
    sam_close(out_sam_fp);
}


std::string make_cmd(int argc, char** argv) {
    std::string cmd("");
    for (auto i = 0; i < argc; ++i) {
        cmd += std::string(argv[i]) + " ";
    }
    return cmd;
}

/* This is a wrapper for the constructor of LiftMap
 */
lift::LiftMap lift::lift_from_vcf(
    std::string fname, std::string sample, 
    std::string haplotype, 
    NameMap names, LengthMap lengths
) {
    if (fname == "" && sample == "") {
        fprintf(stderr, "vcf file name and sample name are required!! \n");
        print_serialize_help_msg();
        exit(1);
    }
    vcfFile* fp = bcf_open(fname.data(), "r");
    bcf_hdr_t* hdr = bcf_hdr_read(fp);
    return lift::LiftMap(fp, hdr, sample, haplotype, names, lengths);
}


void print_serialize_help_msg(){
    fprintf(stderr, "\n");
    fprintf(stderr, "Index a lift-over map using either a VCF or a chain file.\n");
    fprintf(stderr, "Usage:   leviosam serialize [options] {-v <vcf> | -c <chain>}     or\n");
    fprintf(stderr, "         leviosam index [options] {-v <vcf> | -c <chain>}\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "         VcfMap options:\n");
    fprintf(stderr, "           -v string Index a lift-over map from a VCF file.\n");
    fprintf(stderr, "           -s string The sample used to build leviosam index (-v needs to be set).\n");
    fprintf(stderr, "           -g 0/1    The haplotype used to index leviosam. [0] \n");
    fprintf(stderr, "           -n string Path to a name map file.\n");
    fprintf(stderr, "                     This can be used to map '1' to 'chr1', or vice versa.\n");
    fprintf(stderr, "           -k string Path to a length map file.\n");
    fprintf(stderr, "         ChainMap options:\n");
    fprintf(stderr, "           -c string Index a lift-over map from a chain file.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "         -p string The prefix of the output file.\n");
    fprintf(stderr, "         -O format Output file format, can be sam or bam. [sam]\n");
    fprintf(stderr, "\n");
}

void print_lift_help_msg(){
    fprintf(stderr, "\n");
    fprintf(stderr, "Perform efficient lift-over using levioSAM.\n");
    fprintf(stderr, "Usage:   leviosam lift [options] {-v <vcf> | -l <vcfmap> | -c <chain> | -C <chainmap>}\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "         -a string Path to the SAM/BAM file to be lifted. \n");
    fprintf(stderr, "                   Leave empty or set to \"-\" to read from stdin.\n");
    fprintf(stderr, "         -t INT    Number of threads used. [1] \n");
    fprintf(stderr, "         -T INT    Chunk size for each thread. [256] \n");
    fprintf(stderr, "                   Each thread queries <-T> reads, lifts, and writes.\n");
    fprintf(stderr, "                   Setting a higher <-T> uses slightly more memory but might benefit thread scaling.\n");
    fprintf(stderr, "         -m        add MD and NM to output alignment records (requires -f option)\n");
    fprintf(stderr, "         -f string Fasta reference that corresponds to input SAM/BAM (for use w/ -m option)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "         VcfMap options (one of -v or -l must be set to perform lift-over using a VcfMap):\n");
    fprintf(stderr, "           -v string If -l is not specified, can build indexes using a VCF file.\n");
    fprintf(stderr, "           -l string Path to an indexed VcfMap.\n");
    fprintf(stderr, "         ChainMap options (one of -c and -C must be set to perform lift-over using a ChainMap):\n");
    fprintf(stderr, "           -c string If -C is not specified, build a ChainMap from a chain file.\n");
    fprintf(stderr, "           -C string Path to an indexed ChainMap.\n");
    fprintf(stderr, "           -G INT    Number of allowed CIGAR changes for one alingment. [10]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "         Commit/defer rule options:\n");
    fprintf(stderr, "           Example: `-S mapq,aln_score -M 20 -A 20` commits MQ>=20 and AS>=20 alignments.\n");
    fprintf(stderr, "           -S string Split the aligned reads with an indicator. Options: mapq, aln_score, isize, clipped_frac. [none]\n");
    fprintf(stderr, "           -M int    Min MAPQ to commit (pre-liftover; must with `-S mapq`). [10]\n");
    fprintf(stderr, "           -A int    Min AS:i to commit (pre-liftover; must with `-S aln_score`). [100]\n");
    fprintf(stderr, "           -Z int    Max TLEN/isize to commit (post-liftover; must with `-S isize`). [1000]\n");
    fprintf(stderr, "           -L float  Min fraction of clipped to commit (post-liftover; must with `-S aln_score`). [0.95]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "         The options for serialize can also be used here, if -v/-c is set.\n");
    fprintf(stderr, "\n");
}

void print_main_help_msg(){
    fprintf(stderr, "\n");
    fprintf(stderr, "Program: leviosam (lift over alignments)\n");
    fprintf(stderr, "Version: %s\n", VERSION);
    fprintf(stderr, "Usage:   leviosam <command> [options]\n\n");
    fprintf(stderr, "Commands:serialize   Index a lift-over map.\n");
    fprintf(stderr, "         lift        Lift alignments.\n");
    fprintf(stderr, "Options: -h          Print detailed usage.\n");
    fprintf(stderr, "         -V          Verbose level [0].\n");
    fprintf(stderr, "\n");
}

int main(int argc, char** argv) {
    int c;
    lift_opts args;
    args.cmd = make_cmd(argc,argv);
    static struct option long_options[] {
        // {"md", required_argument, 0, 'm'},
        {"md", no_argument, 0, 'm'},
        {"sam", required_argument, 0, 'a'},
        {"min_aln_score", required_argument, 0, 'A'},
        {"chain", required_argument, 0, 'c'},
        {"chainmap", required_argument, 0, 'C'},
        {"reference", required_argument, 0, 'f'},
        {"haplotype", required_argument, 0, 'g'},
        {"allowed_cigar_changes", required_argument, 0, 'G'},
        {"leviosam", required_argument, 0, 'l'},
        {"max_clipped_frac", required_argument, 0, 'L'},
        {"min_mapq", required_argument, 0, 'M'},
        {"namemap", required_argument, 0, 'n'},
        {"out_format", required_argument, 0, 'O'},
        {"prefix", required_argument, 0, 'p'},
        {"sample", required_argument, 0, 's'},
        {"split_mode", required_argument, 0, 'S'},
        {"threads", required_argument, 0, 't'},
        {"chunk_size", required_argument, 0, 'T'},
        {"vcf", required_argument, 0, 'v'},
        {"verbose", required_argument, 0, 'V'},
        {"max_isize", required_argument, 0, 'Z'}
    };
    int long_index = 0;
    while(
        (c = getopt_long(
            argc, argv,
            "hma:A:c:C:f:g:G:k:l:L:M:n:O:p:s:S:t:T:v:V:Z:",
            long_options, &long_index)) != -1) {
        switch (c) {
            case 'h':
                print_main_help_msg();
                print_serialize_help_msg();
                print_lift_help_msg();
                exit(0);
            case 'm':
                args.md_flag |= 8;
                args.md_flag |= 16;
                break;
            case 'a':
                args.sam_fname = optarg;
                break;
            case 'A':
                args.min_aln_score = atoi(optarg);
                break;
            case 'c':
                args.chain_fname = optarg;
                break;
            case 'C':
                args.chainmap_fname = optarg;
                break;
            case 'f':
                args.ref_name = std::string(optarg);
                break;
            case 'g':
                args.haplotype = optarg;
                break;
            case 'G':
                args.allowed_cigar_changes = atoi(optarg);
                break;
            case 'k':
                args.length_map = parse_length_map(optarg);
                break;
            case 'l':
                args.lift_fname = optarg;
                break;
            case 'L':
                args.max_clipped_frac = atof(optarg);
                break;
            case 'M':
                args.min_mapq = atoi(optarg);
                break;
            case 'n':
                args.name_map = parse_name_map(optarg);
                break;
            case 'O':
                args.out_format = optarg;
                break;
            case 'p':
                args.outpre = optarg;
                break;
            case 's':
                args.sample = optarg;
                break;
            case 'S':
                args.split_mode = optarg;
                break;
            case 't':
                args.threads = atoi(optarg);
                break;
            case 'T':
                args.chunk_size = atoi(optarg);
                break;
            case 'V':
                args.verbose = atoi(optarg);
                break;
            case 'v':
                args.vcf_fname = optarg;
                break;
            case 'Z':
                args.max_isize = atoi(optarg);
                break;
            default:
                fprintf(stderr, "ignoring option %c\n", c);
                exit(1);
        }
    }

    if (argc - optind < 1) {
        fprintf(stderr, "No argument provided\n");
        print_main_help_msg();
        exit(1);
    }
    if (args.haplotype != "0" && args.haplotype != "1"){
        fprintf(stderr, "Invalid haplotype %s\n", args.haplotype.c_str());
        exit(1);
    }
    if (args.out_format != "sam" && args.out_format != "bam") {
        fprintf(stderr, "Not supported extension format %s\n", args.out_format.c_str());
        exit(1);
    }

    if (args.split_mode != "") {
        std::vector<std::string> split_options {"mapq", "clipped_frac", "isize", "aln_score"};
        std::vector<std::string> sm = LevioSamUtils::split_str(args.split_mode, ",");
        for (auto& m: sm) {
            auto cnt = std::count(split_options.begin(), split_options.end(), m);
            if (cnt != 1) {
                std::cerr << "Error: " << m << " is not a valid filtering option\n";
                std::cerr << "Valid options:\n";
                for (auto& opt: split_options) {
                    std::cerr << " - " << opt << "\n";
                }
                exit(1);
            }
        }
    }

    if (!strcmp(argv[optind], "lift")) {
        lift_run(args);
    } else if (!strcmp(argv[optind], "serialize") || !strcmp(argv[optind], "index")) {
        serialize_run(args);
    }
    return 0;
}

