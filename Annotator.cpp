#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include <vector>
#include <pthread.h>

#include "KmerCount.hpp"
#include "SeqSet.hpp"
#include "AlignAlgo.hpp"
#include "FileLineReader.hpp"
#include "ReadFiles.hpp"

char usage[] = "./annotator [OPTIONS]:\n"
		"Required:\n"
		"\t-f STRING: fasta file containing the receptor genome sequence\n"
		"\t-a STRING: path to the assembly file\n"
		"Optional:\n"
		"\t-r STRING: path to the reads used in the assembly\n"
		"\t--fasta: the assembly file is in fasta format (default: false)\n"
		"\t--fastq: the assembly file is in fastq format (default: false)\n"
		"\t-t INT: number of threads (default: 1)\n"
		"\t-o STRING: the prefix of the file containing CDR3 information (default: trust)\n"
		//"\t--partial: including partial CDR3s in the report (default: false)\n"
		"\t--barcode: there is barcode information in -a and -r files (default: not set)\n"
		"\t--UMI: there is UMI information in -r file (default: not set)\n"
		"\t--geneAlignment: output the gene alignment (default: not set)\n"
    "\t--airrAlignment: output the aligned sequences to prefix_airr_align.tsv (default: not set)\n"
		"\t--noImpute: do not impute CDR3 sequence for TCR (default: not set (impute))\n"
		"\t--notIMGT: the receptor genome sequence is not in IMGT format (default: not set(in IMGT format))\n"
		"\t--outputCDR3File: output CDR3 file when not using -r option (default: no output)\n"
		"\t--needReverseComplement: reverse complement sequences on another strand (default: no)\n"
    "\t--outputFormat INT: 0-fasta, 1-AIRR. (default: 0 (fasta))\n" 
		"\t--readAssignment STRING: output the read assignment to the file (default: no output)\n";

int nucToNum[26] = { 0, -1, 1, -1, -1, -1, 2, 
	-1, -1, -1, -1, -1, -1, 0,
	-1, -1, -1, -1, -1, 3,
	-1, -1, -1, -1, -1, -1 } ;

char numToNuc[26] = {'A', 'C', 'G', 'T'} ;

char buffer[10241] = "" ;
char bufferHeader[10241] = "" ;

static const char *short_options = "f:a:r:p:o:t:" ;
static struct option long_options[] = {
			{ "fasta", no_argument, 0, 10000 },
			{ "radius", required_argument, 0, 10001 },
			{ "partial",no_argument, 0, 10002 },
			{ "notIMGT", no_argument, 0, 10003 },
			{ "noImpute", no_argument, 0, 10004 },
			{ "geneAlignment", no_argument, 0, 10005 },
			{ "barcode", no_argument, 0, 10006 },
			{ "UMI", no_argument, 0, 10007 },
			{ "outputCDR3File", no_argument, 0, 10008},
			{ "readAssignment",required_argument, 0, 10009 },
			{ "needReverseComplement", no_argument, 0, 10010 },
			{ "fastq", no_argument, 0, 10011 },
			{ "airrAlignment", no_argument, 0, 10012 }, 
			{ "outputFormat", required_argument, 0, 10013 }, 
			{ (char *)0, 0, 0, 0} 
			} ;

struct _annotate
{
	struct _overlap geneOverlap[4] ;
	struct _overlap cdr[3] ;
	std::vector<struct _overlap> secondaryGeneOverlaps ;
	
	int isFullLength ;

	_annotate()
	{
		isFullLength = 0 ;
	}
} ;

struct _CDR3info
{
	char *seq ;
	double count ;
	double TPM ;
} ;

struct _annotateReadsThreadArg
{
	int tid ;
	int threadCnt ;
	bool impute ;
	bool needRC ;

	SeqSet *seqSet ;
	SeqSet *refSet ;
	struct _annotate *annotations ;
} ;

struct _assignReadsThreadArg
{
	int tid ;
	int threadCnt ;

	SeqSet *seqSet ;
	std::vector<struct _assignRead> *pAssembledReads ;
	int assembledReadCnt ;
} ;

void PrintLog( const char *fmt, ... )
{
	va_list args ;
	va_start( args, fmt ) ;
	vsprintf( buffer, fmt, args ) ;

	time_t mytime = time(NULL) ;
	struct tm *localT = localtime( &mytime ) ;
	char stime[500] ;
	strftime( stime, sizeof( stime ), "%c", localT ) ;
	fprintf( stderr, "[%s] %s\n", stime, buffer ) ;
}

char* GetFieldPtr(char *s, const char *field)
{
	char *ret = strstr( s, field ) ;
	if (ret == NULL)
		return ret ;
	return ret += strlen(field) ;
}

bool CompSortAssignedReadsByAssign( const struct _assignRead &a, const struct _assignRead &b )
{
	if ( a.overlap.seqIdx != b.overlap.seqIdx )
		return a.overlap.seqIdx < b.overlap.seqIdx ;
	else
		return strcmp( a.id, b.id ) < 0 ;
}

// We already make sure they overlap before calling this function
bool IsCDR3Compatible( const struct _assignRead &r, const struct _CDR3info &cdr3, const struct _overlap cdr3Coord )
{
	int rOffset ;
	int cOffset ;
	if ( r.overlap.seqStart <= cdr3Coord.readStart )
	{
		rOffset = r.overlap.readStart + cdr3Coord.readStart - r.overlap.seqStart ;
		cOffset = 0 ;
	}
	else if ( r.overlap.seqStart > cdr3Coord.readStart ) 
	{
		rOffset = r.overlap.readStart ;
		cOffset = r.overlap.seqStart - cdr3Coord.readStart ;
	}

	int i ;
	for ( i = 0 ; r.read[i + rOffset] && cdr3.seq[i + cOffset] ; ++i )
	{
		if ( r.read[i + rOffset] != cdr3.seq[i + cOffset] )
			return false ;
	}
	//printf( "%s\n%s\n", r.read, cdr3.seq ) ;
	//printf( "%d %d %d\n", i, rOffset, cOffset ) ;
	return true ;
}

bool IsSameFragment(char *id1, char *id2, char *s1, char *s2)
{
	if (!strcmp(id1, id2))
		return true ;
	else
	{
		// The case for merged pairs.
		int l1 = strlen(id1) ;
		int l2 = strlen(id2) ;
		if ((l1 == l2 - 2 && id2[l2-2] == '.' && id2[l2 - 1] == '1')
				|| (l2 == l1 - 2 && id1[l1-2] == '.' && id1[l1 - 1] == '1'))
		{
			if (!strcmp(s1, s2))
				return true ;
		}
	}
	return false ;
}

// Could be improved by using BitTable and compress the read covering the same set of CDR3s.
void AbundanceEstimation( std::vector< SimpleVector<int> > &compat, std::vector< struct _CDR3info >& info )
{
	int i, j, t ;
	double endD = 1e-6 ;
	int cCnt = info.size() ;
	int rCnt = compat.size() ;
	double sum = 0 ;

	SimpleVector<double> abundance ;
	abundance.ExpandTo( cCnt ) ;
	abundance.SetZero( 0, cCnt ) ;

	for ( i = 0 ; i < rCnt ; ++i )
		if ( compat[i].Size() == 1 )
			++abundance[ compat[i][0] ] ;
	for ( i = 0 ; i < cCnt ; ++i )
		sum += abundance[i] ;
	
	if ( sum == 0 && cCnt > 0 ) // This happens when the CDR3s are all created by disagreeed mate-pairs, 
			// in this case, just report one.
	{
		info[0].count = rCnt ;
		for ( i = 1 ; i < cCnt ; ++i )
			info[i].count = 0 ;
		return ;
	}
	
	for ( i = 0 ; i < cCnt ; ++i )
		abundance[i] /= sum ;

	for ( t = 0 ; t < 1000 ; ++t )
	{
		double d = 0 ;
		for ( i = 0 ; i < cCnt ; ++i )
			info[i].count = 0 ;
		// E-step
		for ( i = 0 ; i < rCnt ; ++i )
		{
			int size = compat[i].Size() ;
			sum = 0 ;
			for ( j = 0 ; j < size ; ++j )
			{
				//printf( "%d %d %d\n", i, j, compat[i][j] ) ;
				sum += abundance[ compat[i][j] ] ;
			}
			if ( sum == 0 ) // Some read assignments can be incompatible with any cdr3 due to mate-pair disagreement.
				continue ;
			for ( j = 0 ; j < size ; ++j )
				info[ compat[i][j] ].count += abundance[ compat[i][j] ] / sum ;
		}

		// M-step
		sum = 0 ;
		for ( i = 0 ; i < cCnt ; ++i )
			sum += info[i].count ;
		for ( i = 0 ; i < cCnt ; ++i )
		{
			double tmp = abundance[i] ;
			abundance[i] = info[i].count / sum ;
			double diff = ABS( tmp - abundance[i] ) ;
			if ( diff > d )
				d = diff ;
		}
		//printf( "%lf %lf %lf\n", abundance[0], abundance[1], d ) ;
		if ( d < endD )
			return ;
	}
}

void AnnotationTieBreak( struct _annotate *annotations, SeqSet &seqSet, SeqSet &refSet )
{
	int i, j, k ;
	int seqCnt = seqSet.Size() ;
	int refCnt = refSet.Size() ;

	double *abundance = new double[ refCnt ] ; 
	memset( abundance, 0, sizeof( double ) * refCnt ) ;
	// Collect the abundance
	for ( i = 0 ; i < seqCnt ; ++i )
	{
		int weightSum = seqSet.GetSeqWeightSum( i ) ;
		int len = seqSet.GetSeqConsensusLen( i ) ;
		double avgWeight = (double)weightSum / len ;
		for ( k = 0 ; k < 4 ; ++k )
		{
			if ( annotations[i].geneOverlap[k].seqIdx == -1 )
				continue ;
			abundance[ annotations[i].geneOverlap[k].seqIdx ] += avgWeight ;
		}		
	}

	// Break ties
	for ( i = 0 ; i < seqCnt ; ++i )
	{
		struct _overlap *geneOverlap = annotations[i].geneOverlap ;
		for ( k = 0 ; k < 4 ; ++k )
		{
			if ( geneOverlap[k].seqIdx == -1 )
				continue ;

			std::vector<struct _overlap> &overlaps = annotations[i].secondaryGeneOverlaps ;
			int size = overlaps.size() ;
			for ( j = 0 ; j < size ; ++j )
			{
				if ( refSet.GetGeneType( refSet.GetSeqName( overlaps[j].seqIdx ) ) != k )
					continue ;
				if ( overlaps[j].readEnd - overlaps[j].readStart == geneOverlap[k].readEnd - geneOverlap[k].readStart 
						&& overlaps[j].similarity == geneOverlap[k].similarity 
						&& abundance[ overlaps[j].seqIdx ] > abundance[ geneOverlap[k].seqIdx ] ) 
				{
					struct _overlap tmp ;
					tmp = geneOverlap[k] ;
					geneOverlap[k] = overlaps[j] ;
					overlaps[j] = tmp ;
				}
			}
		}
	}
	delete[] abundance ;
}

int IsFullLengthAssembly(char *seq, struct _annotate &annotation, SeqSet &refSet)
{
	int i ;
	struct _overlap *geneOverlap = annotation.geneOverlap ;
	struct _overlap *cdr = annotation.cdr ;
	for ( i = 0 ; i < 4 ; ++i )
	{
		if ( i == 1 )
			continue ;
		if (geneOverlap[i].seqIdx == -1)
			return 0 ;
	}

  if (cdr[2].seqIdx == -1)
      return 0 ;

	if (geneOverlap[0].readEnd > geneOverlap[2].readStart + 3 
		|| geneOverlap[2].readEnd > geneOverlap[3].readStart + 6 )
		return 0 ;
	if (geneOverlap[0].seqStart > 0 || geneOverlap[0].readEnd < cdr[2].readStart)
		return 0 ;
	if (geneOverlap[2].readStart > cdr[2].readEnd || geneOverlap[2].seqEnd < refSet.GetSeqConsensusLen(geneOverlap[2].seqIdx) - 1)
		return 0 ;
	if (geneOverlap[3].seqStart > 10)
		return 0 ;
	for ( i = geneOverlap[0].readStart ; i <= geneOverlap[3].readEnd ; ++i )
	{
		if (seq[i] == 'N')
			return 0 ;
	}
	return 1 ;
}


void *AnnotateReads_Thread( void *pArg )
{
	struct _annotateReadsThreadArg &arg = *( (struct _annotateReadsThreadArg *)pArg ) ;
	int start, end ;
	int i ;
	int seqCnt = arg.seqSet->Size() ;
	char *buffer = new char[10001] ;
	start = seqCnt / arg.threadCnt * arg.tid ;
	end = seqCnt / arg.threadCnt * ( arg.tid + 1 ) ;
	if ( arg.tid == arg.threadCnt - 1 )
		end = seqCnt ;
	for ( i = start ; i < end ; ++i )
	{
		if ( arg.needRC )
		{
			int strand = arg.refSet->HasHitInSet( arg.seqSet->GetSeqConsensus(i)) ;
			if ( strand == -1 )
				arg.seqSet->ReverseComplementInSeqSet( i ) ;
		}
		arg.refSet->AnnotateRead( arg.seqSet->GetSeqConsensus( i ), 2, arg.annotations[i].geneOverlap, arg.annotations[i].cdr, 
			&( arg.annotations[i].secondaryGeneOverlaps ) ) ;
		if ( arg.impute && arg.refSet->ImputeCDR3( arg.seqSet->GetSeqConsensus( i ), buffer, 
			arg.annotations[i].geneOverlap, arg.annotations[i].cdr, 
			&( arg.annotations[i].secondaryGeneOverlaps ) ) != -1 )
			arg.seqSet->SetSeqConsensus( i, buffer ) ;
	}
	delete[] buffer ;
	pthread_exit( NULL ) ;
}

void *AssignReads_Thread( void *pArg )
{
	struct _assignReadsThreadArg &arg = *( (struct _assignReadsThreadArg *)pArg ) ;
	int start, end ;
	int i ;
	std::vector< struct _assignRead> &assembledReads = *arg.pAssembledReads ;
	start = arg.assembledReadCnt / arg.threadCnt * arg.tid ;
	end = arg.assembledReadCnt / arg.threadCnt * ( arg.tid + 1 ) ;
	if ( arg.tid == arg.threadCnt - 1 )
		end = arg.assembledReadCnt ;
	struct _overlap assign ;
	for ( i = start ; i < end ; ++i )
	{
		if ( i == start || strcmp( assembledReads[i].read, assembledReads[i - 1].read ) )
			arg.seqSet->AssignRead( assembledReads[i].read, assembledReads[i].overlap.strand, 
				assembledReads[i].barcode, assign ) ;
		assembledReads[i].overlap = assign ;	
	}
	pthread_exit( NULL ) ;
}


int main( int argc, char *argv[] )
{
	int i, j, k ;
	int radius = 10 ;

	if ( argc <= 1 )
	{
		fprintf( stderr, "%s", usage ) ;
		return 0 ;
	}

	SeqSet refSet( 7 ) ;
	SeqSet seqSet( 17 ) ;
	int c, option_index ;
	FILE *fpReadAssignment = NULL ;
	option_index = 0 ;
	char outputPrefix[1024] = "trust" ;
	FileLineReader flrReads ;
	int threadCnt = 1 ;
	bool includePartial = true ;
	bool isIMGT = true ;
	bool impute = true ;
	bool outputGeneAlignment = false ;
	bool outputAirrAlignment = false ;
	bool hasBarcode = false ;
	bool hasUmi = false ;
	bool outputCDR3File = false ; // whether output the cdr3 file when the input is fasta
	bool needRC = false ; // need reverse complment
	int format = 0 ; // 0-trust4 format. 1-fasta, 2-fastq
	int outputFormat = 0 ; //0-fasta, 1-airr
  std::map<std::string, int> barcodeStrToInt ;
	std::string assemblyFileName ;

	while ( 1 )
	{
		c = getopt_long( argc, argv, short_options, long_options, &option_index ) ;
		
		if ( c == -1 )
			break ;
		
		if ( c == 'f' )
		{
			//refSet.InputRefFa( optarg ) ;
			strcpy( buffer, optarg ) ;
		}
		else if ( c == 'a' )
		{
			std::string s(optarg) ;
			assemblyFileName = s ;
		}
		else if ( c == 'r' )
		{
			flrReads.Open(optarg) ;
		}
		else if ( c == 'o' )
		{
			strcpy( outputPrefix, optarg ) ;
		}
		else if ( c == 't' )
		{
			threadCnt = atoi( optarg ) ;
		}
		else if ( c == 10000 ) // --fasta
		{
			format = 1 ;
		}
		else if ( c == 10011) // --fastq
		{
			format = 2 ;
		}
		else if ( c == 10001 ) // --radius
		{
			radius = atoi( optarg ) ;
		}
		else if ( c == 10002 ) // --partial. not used
		{
			includePartial = true ;	
		}
		else if ( c == 10003 ) // notIMGT 
		{
			isIMGT = false ;
		}
		else if ( c == 10004 ) // noImpute
		{
			impute = false ;
		}
		else if ( c == 10005 ) // --geneAlignment
		{
			outputGeneAlignment = true ;
		}
		else if ( c == 10006 ) // --barcode
		{
			hasBarcode = true ;
		}
		else if ( c == 10007 ) // --umi
		{
			hasUmi = true ;
		}
		else if ( c == 10008 ) // force output CDR3 file
		{
			outputCDR3File = true ;
		}
		else if ( c == 10009 ) // output read assignment
		{
			fpReadAssignment = fopen(optarg, "w") ;
		}
		else if ( c == 10010 ) // Reads on different strand, need to RC first
		{
			needRC = true ;
		}
		else if ( c == 10012 ) // --airrAlignment
		{
			outputAirrAlignment = true ;
		}
    else if (c == 10013) // outputFormat
    {
      outputFormat = atoi(optarg) ;
    }
		else
		{
			fprintf( stderr, "%s", usage ) ;
			return EXIT_FAILURE ;
		}
	}

	refSet.InputRefFa( buffer, isIMGT ) ;
	//refSet.OutputRef( stdout ) ;
	//return 0 ;

	if ( refSet.Size() == 0 )
	{
		fprintf( stderr, "Need to use -f to specify the receptor genome sequence.\n" ) ;
		return EXIT_FAILURE ;
	}
	
	if ( assemblyFileName.length() == 0 )
	{
		fprintf( stderr, "Need to use -a to specify the assembly file.\n" ) ;
		return EXIT_FAILURE ;
	}
	
	refSet.SetHitLenRequired( 17 ) ;
	refSet.SetRadius( radius ) ;
	PrintLog( "Start to annotate assemblies." ) ;
	if (format == 0)
	{
		FileLineReader flrAssembly ;
		flrAssembly.Open(assemblyFileName.c_str()) ;
		while ( flrAssembly.ReadLine() != NULL )
		{
			strcpy(buffer, flrAssembly.GetLinePtr()) ;

			flrAssembly.ReadLine() ;
			char *seq = strdup( flrAssembly.GetLinePtr() ) ;
			int len = strlen( seq ) ;

			// Read in the four line of pos weight
			SimpleVector<struct _posWeight> posWeight ;
			double depthSum = 0 ;
			posWeight.ExpandTo( len ) ;
			for ( k = 0 ; k < 4 ; ++k )
			{
				flrAssembly.ReadLine() ;
				const char *weightBuffer = flrAssembly.GetLinePtr() ;

				int num = 0 ;
				i = 0 ;
				for ( j = 0 ; weightBuffer[j] && weightBuffer[j] != '\n' ; ++j )
				{
					if ( weightBuffer[j] == ' ' )
					{
						posWeight[i].count[k] = num ;
						depthSum += num ;
						++i ;
						num = 0 ;
					}
					else
						num = num * 10 + weightBuffer[j] - '0' ; 
				}
				posWeight[i].count[k] = num ;
				depthSum += num ;
			}
			for ( i = 0 ; buffer[i] && buffer[i] != '\n' && buffer[i] != ' ' ; ++i )
				;
			buffer[i] = '\0' ;

			seqSet.InputNovelSeq( buffer + 1, seq, posWeight ) ;
			free(seq) ;

		}
		flrAssembly.Close() ;
	}
	else
	{
		ReadFiles readFile ;
		readFile.AddReadFile(assemblyFileName.c_str(), false) ;
		while (readFile.Next())
		{
			seqSet.InputNovelRead( readFile.id, readFile.seq, 1, -1 ) ;
		}
	}

	if ( hasBarcode )
		seqSet.SetBarcodeFromSeqName( barcodeStrToInt ) ;

	int seqCnt = seqSet.Size() ;
	struct _annotate *annotations = new struct _annotate[ seqCnt ] ;
	if ( threadCnt <= 1 )
	{
		for ( i = 0 ; i < seqCnt ; ++i )
		{
			if ( needRC )
			{
				int strand = refSet.HasHitInSet( seqSet.GetSeqConsensus(i) ) ;
				if ( strand == -1 )
					seqSet.ReverseComplementInSeqSet( i ) ;
			}

			refSet.AnnotateRead( seqSet.GetSeqConsensus( i ), 2, annotations[i].geneOverlap, annotations[i].cdr, 
					&annotations[i].secondaryGeneOverlaps ) ;
			
			if ( impute && refSet.ImputeCDR3( seqSet.GetSeqConsensus( i ), buffer, annotations[i].geneOverlap, annotations[i].cdr, 
					&annotations[i].secondaryGeneOverlaps ) != -1 )
			{
				seqSet.SetSeqConsensus( i, buffer ) ;
			}
		}
	}
	else
	{
		pthread_t *threads = new pthread_t[ threadCnt ] ;
		struct _annotateReadsThreadArg *args = new struct _annotateReadsThreadArg[threadCnt] ;
		pthread_attr_t attr ;

		pthread_attr_init( &attr ) ;
		pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE ) ;

		for ( i = 0 ; i < threadCnt ; ++i )
		{
			args[i].tid = i ;
			args[i].threadCnt = threadCnt ;
			args[i].seqSet = &seqSet ;
			args[i].refSet = &refSet ;
			args[i].annotations = annotations ;
			args[i].impute = impute ;
			args[i].needRC = needRC ;
			pthread_create( &threads[i], &attr, AnnotateReads_Thread, (void *)( args + i ) ) ;
		}

		for ( i = 0 ; i < threadCnt ; ++i )
			pthread_join( threads[i], NULL ) ;


		delete[] threads ;
		delete[] args ;
	}
	// Use global information to break ties
	AnnotationTieBreak( annotations, seqSet, refSet ) ;

  // Add other informations for annotation.
	for ( i = 0 ;i < seqCnt ; ++i )
	{
		annotations[i].isFullLength = IsFullLengthAssembly( seqSet.GetSeqConsensus(i), annotations[i], refSet ) ;
	}

	// Output the annotation of consensus assemblies
  if (outputFormat == 1)
  {
    refSet.GetPartAirrHeader(buffer) ;
    printf("sequence_id\t%s\tcomplete_vdj\n", buffer) ;
  }

	for ( i = 0 ; i < seqCnt ; ++i )
	{
		if (outputFormat != 1)
    {
      int weightSum = seqSet.GetSeqWeightSum( i ) ; 
      int len = seqSet.GetSeqConsensusLen( i ) ;
      sprintf( buffer, ">%s %d %.2lf", seqSet.GetSeqName( i ), len, (double)weightSum / 500.0 ) ;
      refSet.AnnotationToString( seqSet.GetSeqConsensus( i ), annotations[i].geneOverlap, 
          annotations[i].cdr, &annotations[i].secondaryGeneOverlaps, outputGeneAlignment, buffer + strlen( buffer ) ) ;
      printf( "%s\n%s\n", buffer, seqSet.GetSeqConsensus( i ) ) ;
    }
    else
    {
      char *airrStr = refSet.AnnotationToAirrString( seqSet.GetSeqConsensus( i ), annotations[i].geneOverlap, annotations[i].cdr) ;
      printf("%s\t%s\t%c\n", seqSet.GetSeqName( i ), 
          airrStr, annotations[i].isFullLength ? 'T' : 'F') ;    
      free(airrStr) ;
    }
	}

	if (outputAirrAlignment)
  {
    FILE *fpAirrAlignment = NULL ;
		sprintf(buffer, "%s_airr_align.tsv", outputPrefix) ;
		fpAirrAlignment = fopen(buffer, "w") ;

    for (i = 0 ; i < seqCnt ; ++i)
    {
      if (annotations[i].cdr[2].seqIdx != -1)
      {
        char *airrAlignStr = refSet.AnnotationToAirrAlign(seqSet.GetSeqConsensus(i), annotations[i].geneOverlap, annotations[i].cdr, true) ;
        fprintf(fpAirrAlignment, "%s\t%s\n", seqSet.GetSeqName(i), airrAlignStr) ;
        free(airrAlignStr) ;
      }
    }
		fclose( fpAirrAlignment ) ;
  }
		
	// Output more CDR3 information 
	if ( flrReads.IsOpen() )
	{
		struct _overlap assign ;
		std::vector< std::vector<struct _CDR3info> > cdr3Infos ;
		cdr3Infos.resize( seqCnt ) ;
		int strand, minCnt, medCnt ;
		k = 0 ;
		PrintLog( "Start to realign reads for CDR3 analysis." ) ;
		seqSet.Clean( false ) ;

		std::vector<struct _assignRead> cdr3Reads ; // Keep the information of the reads aligned to cdr3 region.
		std::vector<struct _assignRead> assembledReads ;
		int assembledReadCnt ;
		
		//while ( fscanf( fpReads, "%s %d %d %d", buffer, &strand, &minCnt, &medCnt ) != EOF )	
		while ( flrReads.ReadLine() != NULL )
		{
			strcpy(bufferHeader, flrReads.GetLinePtr()) ;
			sscanf( bufferHeader, "%s %d %d %d", buffer, &strand, &minCnt, &medCnt ) ;
			//fscanf( fpReads, "%s", seq ) ; 
			flrReads.ReadLine() ;
			
			struct _assignRead nr ;
			nr.barcode = -1 ;
			nr.umi = -1 ;

			if ( hasBarcode )
			{
				char *p = GetFieldPtr( bufferHeader, " barcode:" ) ;	
				char barcodeBuffer[1024] ;
				if (p == NULL)
				{
					fprintf( stderr, "%s has not barcode field\n", bufferHeader) ;
					exit(1) ;
				}
				for ( i = 0 ; *p && *p != ' ' && *p != '\n' ; ++i, ++p )
					barcodeBuffer[i] = *p ;
				barcodeBuffer[i] = '\0' ;
				std::string s(barcodeBuffer) ;
				if ( barcodeStrToInt.find( s ) == barcodeStrToInt.end() )
					continue ;
				nr.barcode = barcodeStrToInt[s] ;
			}

			if ( hasUmi )
			{
				char *p = GetFieldPtr( bufferHeader, " umi:" ) ;
				if ( p != NULL )
					nr.umi = atoi(p) ;
			}

			nr.id = strdup( buffer + 1 ) ; // skip the > sign
			nr.read = strdup( flrReads.GetLinePtr() ) ;
			nr.overlap.strand = strand ;
			assembledReads.push_back( nr ) ;
		}
		assembledReadCnt = assembledReads.size() ;
		
		int longReadCnt = 0 ;
		for ( i = 0 ; i < assembledReadCnt ; ++i )
		{
			if ( strlen( assembledReads[i].read ) >= 200 )
				++longReadCnt ;
		}
		if ( longReadCnt > assembledReadCnt / 2 )
		{
			seqSet.SetIsLongSeqSet( true ) ;
		}
		
		if ( threadCnt <= 1 )
		{
			for ( i = 0 ; i < assembledReadCnt ; ++i )
			{
				if ( i == 0 || strcmp( assembledReads[i].read, assembledReads[i - 1].read ) ) 
					seqSet.AssignRead( assembledReads[i].read, assembledReads[i].overlap.strand, 
						assembledReads[i].barcode, assign ) ;	
				assembledReads[i].overlap = assign ;	
				if ( ( i + 1 ) % 100000 == 0 )
				{
					PrintLog( "Realigned %d reads.", i + 1 ) ;
				}
			}
		}
		else
		{
			pthread_t *threads = new pthread_t[ threadCnt ] ;
			struct _assignReadsThreadArg *args = new struct _assignReadsThreadArg[threadCnt] ;
			pthread_attr_t attr ;

			pthread_attr_init( &attr ) ;
			pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_JOINABLE ) ;

			for ( i = 0 ; i < threadCnt ; ++i )
			{
				args[i].tid = i ;
				args[i].threadCnt = threadCnt ;
				args[i].seqSet = &seqSet ;
				args[i].pAssembledReads = &assembledReads ;
				args[i].assembledReadCnt = assembledReadCnt ;
				pthread_create( &threads[i], &attr, AssignReads_Thread, (void *)( args + i ) ) ;
			}

			for ( i = 0 ; i < threadCnt ; ++i )
				pthread_join( threads[i], NULL ) ;


			delete[] threads ;
			delete[] args ;
		}

		if ( fpReadAssignment )
		{
			// Output the read assignment information
			for ( i = 0 ; i < assembledReadCnt ; ++i )
			{
				if (assembledReads[i].overlap.seqIdx == -1)
					continue ;
				fprintf( fpReadAssignment, "%s\t%s\n", assembledReads[i].id, seqSet.GetSeqName(assembledReads[i].overlap.seqIdx) ) ;
			}
			fclose(fpReadAssignment) ;
		}

		for ( i = 0 ; i < assembledReadCnt ; ++i )
		{
			assign = assembledReads[i].overlap ;
			if ( assign.seqIdx == -1 )
				continue ;
						
			int cdr3Len =  annotations[ assign.seqIdx ].cdr[2].readEnd -
				annotations[ assign.seqIdx ].cdr[2].readStart + 1 ;
			
			// Store the read overlap with CDR3 region 
			if ( annotations[ assign.seqIdx ].cdr[2].seqIdx != -1 
				&& assign.seqEnd > annotations[ assign.seqIdx ].cdr[2].readStart + 3
				&& assign.seqStart < annotations[ assign.seqIdx ].cdr[2].readEnd - 3 )
			{
				struct  _assignRead nr ;
				nr.id = assembledReads[i].id ; 
				nr.read = strdup( assembledReads[i].read ) ;
				nr.overlap = assign ;
				nr.umi = assembledReads[i].umi ;

				if ( assign.strand == -1 )
				{
					seqSet.ReverseComplement( nr.read, assembledReads[i].read, strlen( nr.read ) ) ;
					nr.overlap.strand = 1 ;
				}
				cdr3Reads.push_back( nr ) ;
			}
			
			// Process the CDR3 region
			if ( !hasBarcode && annotations[assign.seqIdx].cdr[2].seqIdx != -1 
					&& assign.seqStart <= annotations[ assign.seqIdx ].cdr[2].readStart 
					&& assign.seqEnd >=  annotations[ assign.seqIdx ].cdr[2].readEnd )
			{
				std::vector<struct _CDR3info> &info = cdr3Infos[ assign.seqIdx ] ;
				int size = info.size() ;
				
				char *seq = assembledReads[i].read ;
				int offset = assign.readStart +
					annotations[ assign.seqIdx ].cdr[2].readStart - assign.seqStart ;
				if ( assign.strand == 1 )
				{
					memcpy( buffer, seq + offset, sizeof( char ) * cdr3Len ) ;
				}
				else if ( assign.strand == -1 )
				{
					int len = strlen( seq ) ;
					seqSet.ReverseComplement( buffer, seq + ( len - 1 - offset ) - cdr3Len + 1, cdr3Len ) ;
				}
				else
					continue ;
				buffer[cdr3Len] = '\0' ;

				for ( j = 0 ; j < size ; ++j )
					if ( !strcmp( info[j].seq, buffer ) )
					{
						++info[j].count ;
						break ;
					}
				if ( j >= size )
				{
					struct _CDR3info nc ;
					nc.seq = strdup( buffer ) ;
					nc.count = 1 ;

					info.push_back( nc ) ;
				}
				//printf( "%s\n%s %d\n", cdr3Reads[ cdr3Reads.size() - 1 ].read, buffer, i ) ;
			}
		}
		
		// Compute the abundance for each CDR3.
		// If there is no read span the whole CDR3 region, use consensus.
		PrintLog( "Compute CDR3 abundance." ) ;
		for ( i = 0 ; i < seqCnt ; ++i )
		{
			if ( annotations[i].cdr[2].seqIdx == -1 )
				continue ;

			std::vector<struct _CDR3info> &info = cdr3Infos[i] ;
			int size = info.size() ;
			if ( size == 0 )
			{
				struct _CDR3info nc ;
				int len =  annotations[i].cdr[2].readEnd - annotations[i].cdr[2].readStart + 1 ;
				nc.seq = (char *)malloc( sizeof( char) * ( len + 1 ) ) ;
				memcpy( nc.seq, seqSet.GetSeqConsensus( i ) + annotations[i].cdr[2].readStart, len ) ;
				nc.seq[ len ] = '\0' ;
				nc.count = 1 ;

				info.push_back( nc ) ;
			}
		}

		// Distribute the reads.
		std::sort( cdr3Reads.begin(), cdr3Reads.end(), CompSortAssignedReadsByAssign  ) ;
		int cdr3ReadCnt = cdr3Reads.size() ;
		for ( i = 0 ; i < cdr3ReadCnt ; )
		{
			for ( j = i + 1 ; j < cdr3ReadCnt ; ++j )
			{
				if ( cdr3Reads[j].overlap.seqIdx != cdr3Reads[i].overlap.seqIdx )
					break ;
			}
			
			std::vector<struct _CDR3info> &info = cdr3Infos[ cdr3Reads[i].overlap.seqIdx ] ;
			int size = info.size() ;
			std::map<int, int> umiUsed ;
			if ( size == 1 ) // barcode option also forces to get in this branch. 
			{
				int cnt = 0 ;
				for ( k = i ; k < j ; ++k )
				{
					if ( k < j - 1 && IsSameFragment( cdr3Reads[k].id, cdr3Reads[k + 1].id, cdr3Reads[k].read, cdr3Reads[k + 1].read ) )
						++k ;
					
					if ( hasUmi )
					{
						if ( umiUsed.find( cdr3Reads[k].umi ) != umiUsed.end() )
							continue ;
						umiUsed[ cdr3Reads[k].umi ] = 1 ;
					}
					++cnt ;
				}
				info[0].count = cnt ;
				i = j ;
				continue ;
			}
			// Create the compatility relation
			std::vector< SimpleVector<int> > compat ; // read k is compatible to CDR3 l
			for ( k = i ; k < j ; ++k )
			{
				int l ;
				SimpleVector<int> nc ;
				
				if ( hasUmi )
				{
						if ( umiUsed.find( cdr3Reads[k].umi ) != umiUsed.end() )
							continue ;
						umiUsed[ cdr3Reads[k].umi ] = 1 ;
				}

				//if ( k < j - 1 && !strcmp( cdr3Reads[k].id, cdr3Reads[k + 1].id ) )
				if ( k < j - 1 && IsSameFragment( cdr3Reads[k].id, cdr3Reads[k + 1].id, cdr3Reads[k].read, cdr3Reads[k + 1].read ) )
				{
					// Fragment overlap with the region.
					for ( l = 0 ; l < size ; ++l )
					{
						if ( IsCDR3Compatible( cdr3Reads[k], info[l], annotations[ cdr3Reads[i].overlap.seqIdx ].cdr[2] ) 
							&& IsCDR3Compatible( cdr3Reads[k + 1], info[l], 
									annotations[ cdr3Reads[i].overlap.seqIdx ].cdr[2] ) )
						{
							nc.PushBack( l ) ;
						}
					}
					++k ;
				}
				else
				{
					for ( l = 0 ; l < size ; ++l )
					{
						if ( IsCDR3Compatible( cdr3Reads[k], info[l], annotations[ cdr3Reads[i].overlap.seqIdx ].cdr[2] ) )
						{
							//printf( "%d=>%d\n", k, l ) ;
							nc.PushBack( l ) ;
						}
					}
				}

				compat.push_back( nc ) ;
			}
			// EM algorithm to estimate the count
			/*if ( cdr3Reads[i].overlap.seqIdx == 178 )
			{
				int l ;
				for ( l = 0 ; l < info.size() ; ++l )
				{
					fprintf( stderr, "%s %lf\n", info[l].seq, info[l].count ) ;
				}
				for ( int ll = i ; ll < j ; ++ll )
				{
					for ( l = 0 ; l < compat[ll - i].Size() ; ++l )
						fprintf( stderr, "%d ", compat[ll - i][l]) ;
					fprintf( stderr, "\n" ) ;
				}
			}*/	
			AbundanceEstimation( compat, info ) ;

			i = j ;
		}

		// Output different CDR3s from each main assembly.
		sprintf( buffer, "%s_cdr3.out", outputPrefix ) ;
		FILE *fpOutput = fopen( buffer, "w" ) ;
		for ( i = 0 ; i < seqCnt ; ++i )
		{
			if ( annotations[i].cdr[2].seqIdx == -1 )
				continue ;

			if ( !includePartial && annotations[i].cdr[2].similarity == 0 )
				continue ;
			std::vector<struct _CDR3info> &info = cdr3Infos[i] ;
			int size = info.size() ;
			// Output each CDR3 information
			int effectiveJ = 0 ;
			for ( j = 0 ; j < size ; ++j )
			{
				if ( info[j].count == 0 )
					continue ;
				fprintf( fpOutput, "%s\t%d\t", seqSet.GetSeqName( i ), effectiveJ ) ;
				++effectiveJ ;
				// The gene ids
				for ( k = 0 ; k < 4 ; ++k )
				{
					//if ( k == 1 )
					//	continue ;
					if ( annotations[i].geneOverlap[k].seqIdx == -1 )
						fprintf( fpOutput, "*\t" ) ;	
					else
					{
						fprintf( fpOutput, "%s", 
							refSet.GetSeqName( annotations[i].geneOverlap[k].seqIdx ) ) ;
						std::vector<int> secondaryOverlapIdx ;
						int size = refSet.GetEqualSecondaryGeneOverlap( annotations[i].geneOverlap[k],
							k, &annotations[i].secondaryGeneOverlaps, secondaryOverlapIdx ) ;
						int l ;
						for ( l = 0 ; l < size ; ++l )
						{
							int seqIdx = annotations[i].secondaryGeneOverlaps[ secondaryOverlapIdx[l] ].seqIdx ;
							fprintf( fpOutput, ",%s", refSet.GetSeqName( seqIdx ) ) ; 
						}
						fprintf( fpOutput, "\t" ) ;
					}
				}
				// Output CDR1,2
				for ( k = 0 ; k < 2 ; ++k )
				{
					if ( annotations[i].cdr[k].seqIdx == -1 )
						fprintf( fpOutput, "*\t" ) ;
					else
					{
						int len = annotations[i].cdr[k].readEnd - annotations[i].cdr[k].readStart + 1 ;
						memcpy( buffer, seqSet.GetSeqConsensus( i ) + annotations[i].cdr[k].readStart, len ) ;
						buffer[len] = '\0' ;
						fprintf( fpOutput, "%s\t", buffer ) ;
					}
				}
				fprintf( fpOutput, "%s\t%.2lf\t%.2lf\t%.2f\t%d\n", info[j].seq, annotations[i].cdr[2].similarity, info[j].count,
					refSet.GetCDR3Similarity( info[j].seq, annotations[i].geneOverlap, 
						annotations[i].cdr ) * 100.0, annotations[i].isFullLength ) ;
			}
			
		}
		// Free up memory
		for ( i = 0 ; i < cdr3ReadCnt ; ++i )
		{
			free( cdr3Reads[i].read ) ;
		}
		for ( i = 0 ; i < seqCnt ; ++i )
		{
			int size = cdr3Infos[i].size() ;
			for ( j = 0 ; j < size; ++j )
				free( cdr3Infos[i][j].seq ) ;
		}

		for ( i = 0 ; i < assembledReadCnt ; ++i )
		{
			free( assembledReads[i].id ) ;
			free( assembledReads[i].read ) ;
		}

		fclose( fpOutput ) ;
		flrReads.Close() ; 
	}
	else if ( outputCDR3File ) // force output cdr3 file
	{
		// Directly use consensus to output cdr3 information. 
		sprintf( buffer, "%s_cdr3.out", outputPrefix ) ;
		FILE *fpOutput = fopen( buffer, "w" ) ;
		for ( i = 0 ; i < seqCnt ; ++i )
		{
			if ( annotations[i].cdr[2].seqIdx == -1 )
				continue ;

			if ( !includePartial && annotations[i].cdr[2].similarity == 0 )
				continue ;

			fprintf( fpOutput, "%s\t0\t", seqSet.GetSeqName( i ) ) ;
			// The gene ids
			for ( k = 0 ; k < 4 ; ++k )
			{
				//if ( k == 1 )
				//	continue ;
				if ( annotations[i].geneOverlap[k].seqIdx == -1 )
					fprintf( fpOutput, "*\t" ) ;	
				else
				{
					fprintf( fpOutput, "%s", 
							refSet.GetSeqName( annotations[i].geneOverlap[k].seqIdx ) ) ;
					std::vector<int> secondaryOverlapIdx ;
					int size = refSet.GetEqualSecondaryGeneOverlap( annotations[i].geneOverlap[k],
							k, &annotations[i].secondaryGeneOverlaps, secondaryOverlapIdx ) ;
					int l ;
					for ( l = 0 ; l < size ; ++l )
					{
						int seqIdx = annotations[i].secondaryGeneOverlaps[ secondaryOverlapIdx[l] ].seqIdx ;
						fprintf( fpOutput, ",%s", refSet.GetSeqName( seqIdx ) ) ; 
					}
					fprintf( fpOutput, "\t" ) ;
				}
			}
			// Output CDR1,2, 3
			for ( k = 0 ; k < 3 ; ++k )
			{
				if ( annotations[i].cdr[k].seqIdx == -1 )
					fprintf( fpOutput, "*\t" ) ;
				else
				{
					int len = annotations[i].cdr[k].readEnd - annotations[i].cdr[k].readStart + 1 ;
					memcpy( buffer, seqSet.GetSeqConsensus( i ) + annotations[i].cdr[k].readStart, len ) ;
					buffer[len] = '\0' ;
					fprintf( fpOutput, "%s\t", buffer ) ;
				}

				if ( k == 2 )
				{
					// The beginning of for-loop i makes sure buffer has the cdr3 sequence.
					int cov = seqSet.GetConsensusWeightSumRange( i, annotations[i].cdr[2].readStart, 
							annotations[i].cdr[2].readEnd ) ;
					double avgCov = (double)cov / ( annotations[i].cdr[2].readEnd - annotations[i].cdr[2].readStart + 1 ) ;
					fprintf( fpOutput, "%.2lf\t%.2lf\t%.2lf\t%d\n", annotations[i].cdr[2].similarity, avgCov,
						refSet.GetCDR3Similarity( buffer, annotations[i].geneOverlap, 
						annotations[i].cdr ) * 100.0, annotations[i].isFullLength ) ;
				}
			}
		}
		fclose( fpOutput ) ;
	}
	
	delete[] annotations ;
	PrintLog( "Finish annotation." ) ;
	return 0 ;
}
