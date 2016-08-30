#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

/* tardis headers */
#include "processbam.h"

void fix_n_base( char *ref, char *read){
	int i;
	int len = strlen(ref);
	for(i=0; i<len; i++){
		if(ref[i] == 'N')
			ref[i] = read[i];
	}
}

void load_bam( bam_info* in_bam, char* path)
{
	/* Variables */
	htsFile* bam_file;
	bam_hdr_t* bam_header;
	hts_idx_t* bam_index;

	fprintf( stderr, "Processing BAM file %s.\n", path);

	/* Open the BAM file for reading. htslib automatically detects the format
		of the file, so appending "b" after "r" in mode is redundant. */
	bam_file = safe_hts_open( path, "r");
	bam_index = sam_index_load( bam_file, path);

	if (bam_index == NULL){
		fprintf(stderr, "BAM index not found.\n");
		exit (EXIT_COMMON);
	}
	/* Read in BAM header information */
	bam_header = bam_hdr_read( ( bam_file->fp).bgzf);

	get_sample_name( in_bam, bam_header->text);
	in_bam->bam_file = bam_file;
	in_bam->bam_index = bam_index;
	in_bam->bam_header = bam_header;

}

void read_alignment( bam_info* in_bam, parameters *params)
{
	bam1_core_t bam_alignment_core;
	bam1_t*	bam_alignment;
	hts_idx_t* bam_index;
	int return_value;
	int i;
	int j;

	char sequence[MAX_SEQ];
	char read[MAX_SEQ];
	char qual[MAX_SEQ];
	char read2[MAX_SEQ];

	SHA256_CTX ctx;
	BYTE hash_input_read[MAX_SEQ];
	BYTE buf[SHA256_BLOCK_SIZE];
	BYTE hash_bam[SHA256_BLOCK_SIZE] = {0x0};

	int read_len;
	int ref_len;

	char next_char;
	const uint32_t *cigar;
	int n_cigar;
	htsFile *bam_file;

	bam_hdr_t* bam_header;
	char rand_loc[MAX_SEQ];
	int result;
	char md[MAX_SEQ];
	int chrom_id;
	int start; int end;
	char map_chr[MAX_SEQ];
	int map_tid;
	int map_loc;
	/* char *ref_seq; */
	char ref_seq[MAX_SEQ];
	char ref_seq2[MAX_SEQ];
	int loc_len;
	int soft_clips[2] = {0};
	int cigar_add_len;
	int clipped;
	int aligned_read_count=0;
	int reversed = 0;

	bam_file = in_bam->bam_file;
	bam_header = in_bam->bam_header;
	bam_index = in_bam->bam_index;

	bam_alignment = bam_init1();
	j=0;

	if (bam_index == NULL){
		fprintf(stderr, "BAM index not found.\n");
		exit (EXIT_COMMON);
	}

	if (bam_header == NULL){
		fprintf(stderr, "BAM header not found.\n");
		exit (EXIT_COMMON);
	}

	return_value = bam_read1( ( bam_file->fp).bgzf, bam_alignment);

	while( return_value != -1 ){

		bam_alignment_core = bam_alignment->core;

		if (bam_alignment_core.flag & (BAM_FSECONDARY|BAM_FSUPPLEMENTARY)) // skip secondary, supplementary and unmapped alignments
		{
			aligned_read_count++;
			return_value = bam_read1( ( bam_file->fp).bgzf, bam_alignment);
			continue;
		}

		reversed = bam_alignment_core.flag & BAM_FREVERSE;

		// Pull out the cigar field from the alignment
		cigar = bam_get_cigar(bam_alignment);
		// Number of cigar operations
		n_cigar = bam_alignment_core.n_cigar;

		// Copy the whole sequence into char array
		strncpy( sequence, bam_get_seq( bam_alignment), bam_alignment_core.l_qseq);
		sequence[bam_alignment_core.l_qseq] = '\0';
		// Copy the quality string
		strncpy( qual, bam_get_qual( bam_alignment), bam_alignment_core.l_qseq);
		qual[bam_alignment_core.l_qseq] = '\0';
		qual_to_ascii(qual);

		int seq_len = strlen( sequence);
		for( i = 0; i < seq_len; i++){
			read[i] = base_as_char( bam_seqi( sequence, i));
			if(reversed){
				hash_input_read[(seq_len-1)-i] = complement_char(read[i]);
			}
			else{
				hash_input_read[i] = read[i];
			}
		}
		read[i] = '\0';
		read_len = i;

		sha256_init(&ctx);
		sha256_update(&ctx, hash_input_read, read_len);
		sha256_final(&ctx, buf);

		for( i = 0; i < SHA256_BLOCK_SIZE; i++){
			hash_bam[i] += buf[i];
		}
		
		strcpy(read2, read);

		// Need to look after hashing
		if (bam_aux_get(bam_alignment, "MD") == NULL){
			return_value = bam_read1( ( bam_file->fp).bgzf, bam_alignment);
			continue;
		}

		clipped=0;
		cigar_add_len = 0;

		//		//fprintf(stdout, "\nCIGAR: ");
		for (i=0; i<n_cigar; i++){
			if (bam_cigar_opchr(cigar[i]) == 'H'){
				return_value = bam_read1( ( bam_file->fp).bgzf, bam_alignment);

				clipped = 1;
				break;
			}
			else if (bam_cigar_opchr(cigar[i]) == 'D')
				cigar_add_len += bam_cigar_oplen(cigar[i]);
			else if (bam_cigar_opchr(cigar[i]) == 'I')
				cigar_add_len -= bam_cigar_oplen(cigar[i]);
		}

		if (clipped){
			fprintf(stdout, "Hard Clipped\n");
			break;
		}


		strcpy(md, bam_aux_get(bam_alignment, "MD"));

		map_tid = bam_alignment_core.tid;
		map_loc = bam_alignment_core.pos;

		//fprintf(stdout, "map_tid: %d\t map_loc: %d\n", map_tid, map_loc);

		// Get chromosome name to never use again!
		strcpy(map_chr, bam_header->target_name[map_tid]);

		//fprintf(stdout, "map_chr: %s\n", map_chr);

		strncpy(ref_seq, params->chrom_seq[map_tid]+map_loc, read_len+cigar_add_len);
		ref_len = strlen(ref_seq);

		// Sometimes ref chromosome is not long enough to cover the match. This incident should be reported.
		// For now, fill the rest with N bases.
		if(ref_len < (read_len+cigar_add_len)){
			for(i=ref_len; i<read_len+cigar_add_len; i++){
				ref_seq[i] = 'N';
			}
		}
		ref_seq[read_len+cigar_add_len] = '\0';
		strcpy(ref_seq2, ref_seq);

		apply_cigar_md(ref_seq, read, md+1, n_cigar, cigar);

		// Debug fix.
		// Strange condition. Ref genome has N base, MD field suggest different.
		fix_n_base(ref_seq, read);

		if (readcmp(read, ref_seq)){
			fprintf(stdout, "%s\n", bam_get_qname(bam_alignment));
			fprintf(stdout, "%s\n", read);
			fprintf(stdout, "%s\n",	qual);

			fprintf(stdout, "n_cigar: %d\n", n_cigar);
			for (i=0; i<n_cigar; i++){
				fprintf(stdout, "%d\t%c\t%d\t", bam_cigar_oplen(cigar[i]), bam_cigar_opchr(cigar[i]), bam_cigar_type(cigar[i]));
				fprintf(stdout, "%d%c\n", bam_cigar_oplen(cigar[i]), bam_cigar_opchr(cigar[i]));
			}
			fprintf(stdout, "MD: %s\n", md);

			fprintf(stdout, "\npre\n%s\n%s\n", read2, ref_seq2);
			fprintf(stdout, "\npos\n%d %s\n%d %s\n",strlen(read), read, strlen(ref_seq), ref_seq);
			fprintf(stdout, "\ntotal aligned reads\n%d\n", aligned_read_count);
			return;
		}
		else{
			aligned_read_count++;
			//fprintf(stdout, "\nread aligned\n%s\n%s\n", read, ref_seq);
		}

		j++;
		if((j % 100000) == 0){
			fprintf(stdout, "Number of processed reads is %d\r", aligned_read_count);
		}

		return_value = bam_read1( ( bam_file->fp).bgzf, bam_alignment);

	}
	fprintf(stdout, "\n\n");

	BYTE hash_fastq[MAX_SEQ] = {0x0};
	int count = 0;
	for(i=0; i<params->num_fastq_files;i++){
		gzFile fp = gzopen(params->fastq_files[i], "r");
		kseq_t *seq = kseq_init(fp);
		int l;
		while ((l = kseq_read(seq)) >= 0) {
			int k;
			for( k = 0; k < strlen(seq->seq.s); k++){
				hash_input_read[k] = seq->seq.s[k];
			}
			
			sha256_init(&ctx);
			sha256_update(&ctx, hash_input_read, strlen(seq->seq.s));
			sha256_final(&ctx, buf);

			for( k = 0; k < SHA256_BLOCK_SIZE; k++){
				hash_fastq[k] += buf[k];
			}
			count++;
			if((count % 100000) == 0){
				fprintf(stdout, "Calculating hash of fastq files. Processed %d reads\r", count);
			}
		}
	}

	int hash_result = 1;
	for(i=0;i<SHA256_BLOCK_SIZE;i++){
		hash_result = hash_result & (hash_bam[i]==hash_fastq[i]);
	}

	fprintf(stdout, "\nAll reads are matched. Total aligned read count is %d\nBamhash result is %d\n",aligned_read_count, hash_result);
}


void get_sample_name( bam_info* in_bam, char* header_text)
{
	/* Delimit the BAM header text with tabs and newlines */

	char *tmp_header = NULL;
	set_str( &( tmp_header), header_text);
	char* p = strtok( tmp_header, "\t\n");
	char sample_name_buffer[1024];

	while( p != NULL)
	{
		/* If the current token has "SM" as the first two characters,
			we have found our Sample Name */
		if( p[0] == 'S' && p[1] == 'M')
		{
			/* Get the Sample Name */
			strncpy( sample_name_buffer, p + 3, strlen( p) - 3);

			/* Add the NULL terminator */
			sample_name_buffer[strlen( p) - 3] = '\0';

			/* Exit loop */
			break;
		}
		p = strtok( NULL, "\t\n");
	}

	set_str( &( in_bam->sample_name), sample_name_buffer);
	free( tmp_header);
}

int readcmp(char* read1, char* read2){
	int len1 = strlen(read1);
	int len2 = strlen(read2);

	if(len1!=len2){
		return 1;
	}

	int i;
	for(i=0; i<len1; i++){
		if(char_as_base(read1[i]) & char_as_base(read2[i]) <= 0){
			fprintf(stdout, "Not equal: %c %c\n", read1[i], read2[i]);
			return 1;
		}
	}
	return 0;
}
