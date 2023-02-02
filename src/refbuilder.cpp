/*
 * File: refbuilder.cpp
 * Description: Implementation of RefBuilder class
 *              which handles the generation of fasta index, 
 *              and digestion of the input files.
 *
 * Author: Omar Ahmed
 * Start Date: February 12, 2021
 */

#include <refbuilder.hpp>
#include <spumoni_main.hpp>
#include <string>
#include <vector>
#include <iostream>
#include <zlib.h>  
#include <encoder.h>
#include <filesystem>


/* Complement Table from: https://github.com/lh3/seqtk/blob/master/seqtk.c */
char comp_tab[] = {
	  0,   1,	2,	 3,	  4,   5,	6,	 7,	  8,   9,  10,	11,	 12,  13,  14,	15,
	 16,  17,  18,	19,	 20,  21,  22,	23,	 24,  25,  26,	27,	 28,  29,  30,	31,
	 32,  33,  34,	35,	 36,  37,  38,	39,	 40,  41,  42,	43,	 44,  45,  46,	47,
	 48,  49,  50,	51,	 52,  53,  54,	55,	 56,  57,  58,	59,	 60,  61,  62,	63,
	 64, 'T', 'V', 'G', 'H', 'E', 'F', 'C', 'D', 'I', 'J', 'M', 'L', 'K', 'N', 'O',
	'P', 'Q', 'Y', 'S', 'A', 'A', 'B', 'W', 'X', 'R', 'Z',	91,	 92,  93,  94,	95,
	 64, 't', 'v', 'g', 'h', 'e', 'f', 'c', 'd', 'i', 'j', 'm', 'l', 'k', 'n', 'o',
	'p', 'q', 'y', 's', 'a', 'a', 'b', 'w', 'x', 'r', 'z', 123, 124, 125, 126, 127
};

RefBuilder::RefBuilder(const char* ref_file, const char* list_file, const char* output_file, const char* null_reads,  
                       bool build_doc, bool file_list, bool use_minimizers,
                       bool use_promotions, bool use_dna_letters, 
                       size_t k, size_t w, bool use_rev_comp): using_doc(build_doc), using_list(file_list) {
    /* Performs the needed operations to generate a single input file. */

    // Verify every file in the list is valid 
    std::string line = "";
    size_t curr_id = 0, member_num = 0;

    std::ifstream input_fd (list_file, std::ifstream::in);
    std::vector<std::string> input_files;
    std::vector<size_t> document_ids;

    while (std::getline(input_fd, line)) {
        auto word_list = split(line, ' ');

        // Makes sure the files are valid, and there is data to begin with.
        ASSERT((word_list.size() >= 1), "Input file-list does not have expected structure.");
        if (!is_file(word_list[0])) {
            FATAL_ERROR("The following path in the input list is not valid: %s", word_list[0].data());}
        if (!endsWith(word_list[0], ".fa") && !endsWith(word_list[0], ".fasta") && !endsWith(word_list[0], ".fna")) {
            FATAL_ERROR("The following input-file is not a FASTA file: %s", word_list[0].data());}
        input_files.push_back(word_list[0]);

        // If the user wants a document array, there needs to be a ID in second column
        if (using_doc){
            ASSERT((word_list.size() >= 2), "If you want to build a document array, you need a second column with IDs.");

            if (!is_integer(word_list[1])) FATAL_ERROR("A document ID in the file_list is not an integer: %s", word_list[1].data());  
            if (member_num == 0 && std::stoi(word_list[1]) != 1) FATAL_ERROR("The first ID in file_list must be 1");
            
            if (std::stoi(word_list[1]) ==  curr_id || std::stoi(word_list[1]) == (curr_id+1)) {
                if (std::stoi(word_list[1]) == (curr_id+1)) {curr_id+=1;}
                document_ids.push_back(std::stoi(word_list[1]));
            } else {FATAL_ERROR("The IDs in the file_list must be staying constant or increasing by 1.");}
        }
        member_num += 1;
    }
    if (using_doc) {
        ASSERT((document_ids.size() == input_files.size()), "Issue with file-list parsing occurred.");
        if (document_ids.back() == 1) {
            FATAL_WARNING("If you only have one class ID, you should not build a document array.");}
    }

    // Open file to write all the sequences to
    std::ofstream output_fd (output_file, std::ofstream::out);
    std::string mseq = "";

    // Initialize variables needs for over-sampling of reads for null database
    srand(0);
    size_t curr_total_null_reads = 0;
    char grabbed_seq[NULL_READ_CHUNK+1];
    grabbed_seq[NULL_READ_CHUNK] = '\0';

    // Open file for null reads ...
    std::ofstream output_null_fd (null_reads, std::ofstream::out);
    null_read_file = null_reads;

    // Process each input file, and store it forward + reverse complement sequence
    gzFile fp;
    kseq_t* seq;
    std::vector<size_t> seq_lengths;

    curr_id = 1;
    size_t curr_id_seq_length = 0;
    for (auto iter = input_files.begin(); iter != input_files.end(); ++iter) {

        fp = gzopen((*iter).data(), "r");
        seq = kseq_init(fp);
        size_t iter_index = iter-input_files.begin();

        while (kseq_read(seq)>=0) {
            // Get forward seq, and print it
			for (size_t i = 0; i < seq->seq.l; ++i) {
				seq->seq.s[i] = std::toupper(seq->seq.s[i]);
            }

            // Extract some reads for null database generation
            size_t reads_to_grab = (curr_total_null_reads >= NUM_NULL_READS) ? 25 : 100; // downsample if done
            bool go_for_extraction = (curr_total_null_reads < NULL_READ_BOUND);

            for (size_t i = 0; i < reads_to_grab && go_for_extraction && (seq->seq.l > NULL_READ_CHUNK); i++) {
                size_t random_index = rand() % (seq->seq.l-NULL_READ_CHUNK);
                std::strncpy(grabbed_seq, (seq->seq.s+random_index), NULL_READ_CHUNK);

                // Make sure we don't extract reads of Ns
                if (std::string(grabbed_seq).find("N") == std::string::npos) {
                    output_null_fd << ">read_" << curr_total_null_reads << "\n";
                    output_null_fd << grabbed_seq << "\n";
                    curr_total_null_reads++;
                    go_for_extraction = (curr_total_null_reads < NULL_READ_BOUND);
                }
            }

            // Special case when FASTA sequence is less than or equal to 150 bp
            if (seq->seq.l <= NULL_READ_CHUNK) {
                output_null_fd << ">read_" << curr_total_null_reads << "\n";
                output_null_fd << seq->seq.s << "\n";
                curr_total_null_reads++;
            }
            
            // Convert forward_seq to minimizers by default, or save as DNA if asked
            if (use_promotions) {
                mseq = perform_minimizer_digestion(seq->seq.s, k, w);
                output_fd << mseq;
                curr_id_seq_length += mseq.length();
            } else if (use_dna_letters) {
                mseq = perform_dna_minimizer_digestion(seq->seq.s, k, w);
                output_fd << '>' << seq->name.s << '\n' << mseq << '\n';
                curr_id_seq_length += mseq.length();
            } else {
                output_fd << '>' << seq->name.s << '\n' << seq->seq.s << '\n';
                curr_id_seq_length += seq->seq.l;
            }

        
            // Get reverse complement, and print it
            // Based on seqtk reverse complement code, that does it 
            // in place. (https://github.com/lh3/seqtk/blob/master/seqtk.c)
            if (use_rev_comp) {
                int c0, c1;
                for (size_t i = 0; i < seq->seq.l>>1; ++i) { // reverse complement sequence
                    c0 = comp_tab[(int)seq->seq.s[i]];
                    c1 = comp_tab[(int)seq->seq.s[seq->seq.l - 1 - i]];
                    seq->seq.s[i] = c1;
                    seq->seq.s[seq->seq.l - 1 - i] = c0;
                }
                if (seq->seq.l & 1) // complement the remaining base
                    seq->seq.s[seq->seq.l>>1] = comp_tab[(int)seq->seq.s[seq->seq.l>>1]];


                // Convert rev_comp seq to minimizers by default, otherwise DNA
                if (use_promotions) {
                    mseq = perform_minimizer_digestion(seq->seq.s, k, w);
                    output_fd << mseq;
                    curr_id_seq_length += mseq.length();
                } else if (use_dna_letters) {
                    mseq = perform_dna_minimizer_digestion(seq->seq.s, k, w);
                    output_fd << '>' << seq->name.s << '\n' << mseq << '\n';
                    curr_id_seq_length += mseq.length();
                } else {
                    output_fd << '>' << seq->name.s << "_rev_comp" << '\n' << seq->seq.s << '\n';
                    curr_id_seq_length += seq->seq.l;
                }
            }
        }
        kseq_destroy(seq);
        gzclose(fp);

        if (using_doc) {
            // Check if we are transitioning to a new group
            if (iter_index < document_ids.size()-1 && document_ids[iter_index] != document_ids[iter_index+1]){
                seq_lengths.push_back(curr_id_seq_length);
                curr_id += 1; curr_id_seq_length = 0;
            // If last file, output current sequence length
            } else if (iter_index == document_ids.size()-1){
                seq_lengths.push_back(curr_id_seq_length);
                curr_id_seq_length = 0;}
        }
    }
    output_fd.close(); 
    output_null_fd.close();

    // If the reference is empty, issue warning ...
    size_t total_length = std::accumulate(seq_lengths.begin(), seq_lengths.end(), 0);
    if (total_length == 0) {
        std::cout << "\n\n";
        FATAL_WARNING("After sequence digestion, there is no sequence left. "
                      "Note minimizer digestion can only be used with FASTA files.");
    }

    // Assign the full reference to the attribute
    input_file = output_file;
    if (!using_doc) return;
    ASSERT((curr_id == document_ids.back()), "Issue with building the FASTA document index.");

    // Write out the FASTA document index
    std::ofstream output_fdi (input_file + ".fdi", std::ofstream::out);
    for (auto iter = seq_lengths.begin(); iter != seq_lengths.end(); ++iter) {
        size_t iter_index = iter - seq_lengths.begin() + 1;
        output_fdi << "group_" << iter_index << '\t' << *iter << '\n';
    }
    output_fdi.close();
}

const char* RefBuilder::get_ref_path() {
    /* Accessor - return path to full reference that was build */
    return input_file.data();
}

const char* RefBuilder::get_null_readfile() {
    /* Accessor - return path to null reads that were grabbed */
    return null_read_file.data();
}

std::string RefBuilder::parse_null_reads(const char* ref_file, const char* output_path) {
    /* Parses out null reads in the case that we don't use a file-list */

    // Variables for generating null reads ...
    srand(0);
    char grabbed_seq[NULL_READ_CHUNK+1];
    grabbed_seq[NULL_READ_CHUNK] = '\0';

    size_t curr_total_null_reads = 0;
    std::ofstream output_null_fd (output_path, std::ofstream::out);

    // Variables for parsing FASTA ...
    gzFile fp = gzopen(ref_file, "r");
    kseq_t* seq = kseq_init(fp);

    // Go through FASTA file, and extract reads until done
    bool go_for_extraction = (curr_total_null_reads < NULL_READ_BOUND);
    while (kseq_read(seq)>=0 && go_for_extraction) {
        size_t reads_to_grab = (curr_total_null_reads >= NUM_NULL_READS) ? 25 : 100; // downsample if done
        
        for (size_t i = 0; i < reads_to_grab && go_for_extraction && (seq->seq.l > NULL_READ_CHUNK); i++) {
            size_t random_index = rand() % (seq->seq.l-NULL_READ_CHUNK);
            std::strncpy(grabbed_seq, (seq->seq.s+random_index), NULL_READ_CHUNK);

            // Make sure we don't extract reads of Ns
            if (std::string(grabbed_seq).find("N") == std::string::npos) {
                output_null_fd << ">read_" << curr_total_null_reads << "\n";
                output_null_fd << grabbed_seq << "\n";
                curr_total_null_reads++;
                go_for_extraction = (curr_total_null_reads < NULL_READ_BOUND);
            }
        }
    
        // Special case - if sequence is less than or equal to 150 bp 
        if (seq->seq.l <= NULL_READ_CHUNK) {
            output_null_fd << ">read_" << curr_total_null_reads << "\n";
            output_null_fd << seq->seq.s << "\n";
            curr_total_null_reads++;
        }
    }
    kseq_destroy(seq);
    gzclose(fp);
    output_null_fd.close();
    return output_path;
}

std::string RefBuilder::parse_null_reads_from_general_text(const char* ref_file, const char* output_path) {
    /* Parses null reads from general text input reference */

    // Open the reference file, and output file for null reads
    std::ofstream output_fd (output_path, std::ofstream::out);
    gzFile fp = gzopen(ref_file, "r");

    // Variables to help with parsing out null reads
    srand(0);
    size_t batch_size = 10000;
    char grabbed_seq[batch_size+1];
    grabbed_seq[batch_size] = '\0';

    char curr_chunk[NULL_READ_CHUNK+1];
    curr_chunk[NULL_READ_CHUNK] = '\0';

    size_t chunks_written = 0;
    size_t len = 0;

    while (len = gzread(fp, grabbed_seq, batch_size)) {
        // At the end of file
        if (len <= NULL_READ_CHUNK) {
            output_fd << grabbed_seq;
            chunks_written++;
        } else {
            for (size_t i = 0; i < 10 && chunks_written < NULL_READ_BOUND; i++) {
                size_t random_index = rand() % (len-NULL_READ_CHUNK);
                std::strncpy(curr_chunk, (grabbed_seq+random_index), NULL_READ_CHUNK);
                
                output_fd << curr_chunk;
                chunks_written++;
            }
        }
        // Break out of loop if hit bound
        if (chunks_written >= NULL_READ_BOUND)
            break;
    }
    gzclose(fp);
    output_fd.close();
    return output_path;
}

std::string RefBuilder::build_reference(const char* ref_file, const char* output_path,bool use_promotions, bool use_dna_letters,
                                        size_t k, size_t w, bool use_rev_comp) {
    /*
     * Builds the reference file from a single file, it could using either type
     * of minimizer digestion: promotion or DNA. Or just use the original FASTA
     * file but include the reverse complement.
     */
    
    std::ofstream output_fd (output_path, std::ofstream::out);

    // Variables for parsing FASTA ...
    gzFile fp = gzopen(ref_file, "r");
    kseq_t* seq = kseq_init(fp);
    size_t total_length = 0;

    while (kseq_read(seq)>=0) {
        std::string curr_seq = "";

        // Upper-case every letter in the sequence
		for (size_t i = 0; i < seq->seq.l; ++i)
			seq->seq.s[i] = std::toupper(seq->seq.s[i]);

        // Print out the forward seq
        if (use_promotions) {
            curr_seq = perform_minimizer_digestion(seq->seq.s, k, w);
            output_fd << curr_seq; total_length += curr_seq.size();
        } else if (use_dna_letters) {
            curr_seq = perform_dna_minimizer_digestion(seq->seq.s, k, w);
            output_fd << '>' << seq->name.s << '\n' << curr_seq << '\n';
            total_length += curr_seq.size();
        } else {
            output_fd << '>' << seq->name.s << '\n' << seq->seq.s << '\n';
            total_length += seq->seq.l;
        }
        curr_seq = "";

        // Get reverse complement, and print it
        // Based on seqtk reverse complement code, that does it 
        // in place. (https://github.com/lh3/seqtk/blob/master/seqtk.c)
        if (use_rev_comp) {
            int c0, c1;
            for (size_t i = 0; i < seq->seq.l>>1; ++i) { // reverse complement sequence
                c0 = comp_tab[(int)seq->seq.s[i]];
                c1 = comp_tab[(int)seq->seq.s[seq->seq.l - 1 - i]];
                seq->seq.s[i] = c1;
                seq->seq.s[seq->seq.l - 1 - i] = c0;
            }
            if (seq->seq.l & 1) // complement the remaining base
                seq->seq.s[seq->seq.l>>1] = comp_tab[(int)seq->seq.s[seq->seq.l>>1]];

            // Print out the reverse complement sequence
            if (use_promotions) {
                curr_seq = perform_minimizer_digestion(seq->seq.s, k, w);
                output_fd << curr_seq; total_length += curr_seq.size();
            } else if (use_dna_letters) {
                curr_seq = perform_dna_minimizer_digestion(seq->seq.s, k, w);
                output_fd << '>' << seq->name.s << "_rev_comp" <<'\n' << curr_seq << '\n';
                total_length += curr_seq.size();
            } else {
                output_fd << '>' << seq->name.s << "_rev_comp" <<'\n' << seq->seq.s << '\n';
                total_length += seq->seq.l;
            }
        }
    }

    // Check if no sequence has been written ...
    if (total_length == 0) {
        std::cout << "\n\n";
        FATAL_WARNING("After sequence digestion, there is no sequence left. "
                      "Note minimizer digestion can only be used with FASTA files.");
    }

    kseq_destroy(seq);
    gzclose(fp);
    output_fd.close();
    return output_path;
}


