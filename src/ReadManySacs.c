/*****************************************************************************/
/* Set of function dealing with a list of SAC files.                         */
/*                                                                           */
/* Authors: Sergi Ventosa Rahuet (sergiventosa@hotmail.com)                  */
/*****************************************************************************/
/* **** 2020 ****                                                            */
/* Jun25 (1b) Relax criteria to accept SAC files in ReadManySacs()           */
/*  - Sequences having more than N samples are now considered, but cut at    */
/*    sample N.                                                              */
/* Abr13 (1c) The ReadManySacs() now reads only metada when *xOut == NULL    */
/*****************************************************************************/

#include <complex.h>  /* When done before fftw3.h, makes fftw3.h use C99 complex types. */
#include <fftw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <sacio.h>
#include <string.h>
#include <math.h>
#include "ReadManySacs.h"
/*
char *set_utc () {
	char *tz;
	
	tz = getenv ("TZ");
	if (tz) tz = strdup (tz);
	setenv ("TZ", "", 1);
	tzset ();

	return tz;
}

void restor_tz (char *tz) {
	if (tz) setenv ("TZ", tz, 1);
	else unsetenv ("TZ");
	tzset ();
	free (tz);
}

time_t utc_mktime (struct tm *tm) {
	char *tz;
	time_t t;
	
	tz = set_utc ();
	t = mktime (tm);
	restor_tz (tz);
	
	return t;
}
*/
int is_leap(unsigned y) {
	y += 1900;
	return (y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0);
}

time_t my_timegm (struct tm *tm) {
	static const unsigned ndays[2][12] = {
		{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
		{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
	};
	time_t res = 0;
	int i, year;

	year = tm->tm_year + tm->tm_mon/12;
	for (i = 70; i < year; i++)
		res += is_leap(i) ? 366 : 365;
	
	for (i = 0; i < tm->tm_mon%12; i++)
		res += ndays[is_leap(year)][i];
	   
	res += tm->tm_mday - 1;
	res *= 24;
	res += tm->tm_hour;
	res *= 60;
	res += tm->tm_min;
	res *= 60;
	res += tm->tm_sec;
	
	return res;
}

int nerr_print (char *filename, int nerr) {
	printf ("ReadManySacs: Error reading %s header (nerr=%d)\n", filename, nerr); 
	return 2;
}

float **Destroy_FloatArrayList (float **x, unsigned int Tr) {
	unsigned int tr;
	
	if (x != NULL) {
		for (tr=0; tr<Tr; tr++) fftw_free(x[tr]);
		free(x);
	}
	
	return NULL;
}

float **Create_FloatArrayList (unsigned int N, unsigned int Tr) {
	unsigned int tr;
	float **x = NULL;
	
	if (NULL == (x = (float **)calloc(Tr, sizeof(float *)) )) return NULL;
	for (tr=0; tr<Tr; tr++)
		if (NULL == (x[tr] = (float *)fftw_malloc(N * sizeof(float)) ))
			return Destroy_FloatArrayList(x, Tr);
	return x;
}

float **Copy_FloatArrayList (float **x, unsigned int N, unsigned int Tr) {
	unsigned int tr;
	float **y = NULL;
	
	if (NULL == (y = (float **)calloc(Tr, sizeof(float *)) )) return NULL;
	for (tr=0; tr<Tr; tr++) {
		if (NULL == (y[tr] = (float *)fftw_malloc(N * sizeof(float)) ))
			return Destroy_FloatArrayList(x, Tr);
		memcpy(y[tr], x[tr], N*sizeof(float));
	}
	return y;
}

int nerr_OutOfMem_print (char *filename, int npts) {
	printf ("ReadManySacs: Out of memory when reading %s (npts = %d)\n", filename, npts); 
	return 4;
}

int ReadLocation (double *lat, double *lon, char *fin) {
	float stla, stlo;
	char filename[1024], *str1;
	int nerr;
	FILE *fid;
	
	if (NULL == (fid = fopen(fin, "r"))) {
		printf("ReadFirstHeader: cannot open %s\n", fin);
		return -2;
	}
	str1 = fgets(filename, 1024, fid);
	if (str1 == NULL) { 
		printf("ReadFirstHeader: fgets return NULL when reading %s\n", fin);
		return -3;
	}
	str1 = strchr(filename,'\n');    /* Find newline character                 */
	if (str1) *str1 = '\0';          /* to properly set the end of the string. */
	
	fclose (fid);
	
	rsach (filename, &nerr, strlen(filename));
	getfhv ("stla", &stla, &nerr, strlen("stla")); 
	if (nerr) { *lat=0.; nerr = -4; }
	else *lat=(double)stla;
	
	getfhv ("stlo", &stlo, &nerr, strlen("stlo"));
	if (nerr) { *lon=0.; nerr = -4; }
	else *lon=(double)stlo;
	
	return nerr;
}

int ReadManySacs (float **xOut[], t_HeaderInfo *SacHeaderOut[], char **filenamesOut[], unsigned int *TrOut, unsigned int *NOut, float *dtOut, char *fin) {
	t_HeaderInfo *SacHeader=NULL, *phdr1;
	float **x = NULL, *sig=NULL;
	float beg, beg1, dt, dt1;
	unsigned int tr, Tr, N, nskip, npts;
	int nerr, Nmax, ia1;
	char **filenames=NULL, *filename=NULL, *pch;
	/* time_t t1; */
	struct tm tm;
	
	*TrOut = 0;
	// *xOut  = NULL;
	*SacHeaderOut = NULL;
	
	if (fin == NULL) {printf("ReadManySacs: NULL filename\n"); return -1;}
	
	if ( (nerr = CreateFilelist (&filenames, &Tr, fin)) ) {
		printf("ReadManySacs: cannot read %s file (CreateFileList error = %d)\n", fin, nerr);
		return -2;
	}
	
	if (Tr == 0) {
		printf("ReadManySacs: warninng nothing to read for %s.\n", fin);
		DestroyFilelist(filenames);
		return -2;
	}
	
	if (NULL == (SacHeader = (t_HeaderInfo *)calloc(Tr, sizeof(t_HeaderInfo)) )) {
		DestroyFilelist(filenames);
		return 4;
	}
	
	/* Read the header of the first file. */
	filename = filenames[0];
	rsach (filename, &nerr,  strlen(filename));       if (nerr) return nerr_print (filename, nerr);
	getnhv ("npts",  &Nmax,   &nerr, strlen("npts")); if (nerr) return nerr_print (filename, nerr);
	N = (*NOut == 0) ? (unsigned)Nmax : *NOut;
	getfhv ("delta", &dt1,   &nerr, strlen("delta")); if (nerr) return nerr_print (filename, nerr);
	getfhv ("b",     &beg1,  &nerr, strlen("b"));     if (nerr) return nerr_print (filename, nerr);
	
	/* Allocate memory for the input traces */
	Nmax = N;
	if (xOut != NULL) {
		if (NULL == (x = Create_FloatArrayList (N, Tr) )) nerr = 4;
		if (NULL == (sig = (float *)calloc(N, sizeof(float)) )) nerr = 4;
		if (nerr == 4) {
			Destroy_FloatArrayList(x, Tr);
			free(sig);
			free(SacHeader);
			DestroyFilelist(filenames);
			return nerr_OutOfMem_print (filename, N);
		}
	}
	
	/*************************/
	/* Reading and checking. */
	/*************************/
	nskip = 0;
	for (tr=0; tr<Tr; tr++) {
		filename = filenames[tr];
		// Not supported from v102.0
		// sac_warning_off ();

		if (xOut != NULL)
			rsac1(filename, sig, &ia1, &beg, &dt, &Nmax, &nerr, strlen(filename));
		else {
			rsach (filename, &nerr, strlen(filename));       if (nerr) return nerr_print (filename, nerr);
			getnhv ("npts",  &ia1,  &nerr, strlen("npts"));  if (nerr) return nerr_print (filename, nerr);
			getfhv ("delta", &dt,   &nerr, strlen("delta")); if (nerr) return nerr_print (filename, nerr);
			getfhv ("b",     &beg,  &nerr, strlen("b"));     if (nerr) return nerr_print (filename, nerr);
		}
		
		npts = (unsigned)ia1;
		phdr1 = &SacHeader[tr-nskip];
		phdr1->b     = beg;
		phdr1->dt    = dt;
		phdr1->npts  = ia1;

		/* Get a few more sac header fields */
		getnhv ("nzyear", &phdr1->year, &nerr, strlen("nzyear"));
		getnhv ("nzjday", &phdr1->yday, &nerr, strlen("nzjday"));
		getnhv ("nzhour", &phdr1->hour, &nerr, strlen("nzhour"));
		getnhv ("nzmin",  &phdr1->min,  &nerr, strlen("nzmin"));
		getnhv ("nzsec",  &phdr1->sec,  &nerr, strlen("nzsec"));
		getnhv ("nzmsec", &phdr1->msec, &nerr, strlen("nzmsec"));
		getkhv ("knetwk",  phdr1->net,  &nerr, strlen("knetwk"), 8); if (nerr) { phdr1->net[0] = '\0'; nerr = 0; }
		getkhv ("kstnm",   phdr1->sta,  &nerr, strlen("kstnm"),  8); if (nerr) { phdr1->sta[0] = '\0'; nerr = 0; }
		getkhv ("kcmpnm",  phdr1->chn,  &nerr, strlen("kcmpnm"), 8); if (nerr) { phdr1->chn[0] = '\0'; nerr = 0; }
		getkhv ("khole",   phdr1->loc,  &nerr, strlen("khole"),  8); if (nerr) { phdr1->loc[0] = '\0'; nerr = 0; }
		getfhv ("stla",   &phdr1->stla, &nerr, strlen("stla")); if (nerr) { phdr1->nostloc = 1; nerr = 0; }
		getfhv ("stlo",   &phdr1->stlo, &nerr, strlen("stlo")); if (nerr) { phdr1->nostloc = 1; nerr = 0; }
		getfhv ("stel",   &phdr1->stel, &nerr, strlen("stel")); if (nerr) { phdr1->stel = 0; nerr = 0; }
		getfhv ("stdp",   &phdr1->stdp, &nerr, strlen("stdp")); if (nerr) { phdr1->stdp = 0; nerr = 0; }
		getfhv ("cmpaz",  &phdr1->cmpaz,  &nerr, strlen("cmpaz"));  if (nerr) { phdr1->nocmp = 1; nerr = 0; }
		getfhv ("cmpinc", &phdr1->cmpinc, &nerr, strlen("cmpinc")); if (nerr) { phdr1->nocmp = 1; nerr = 0; }
		if ( (pch = memchr(phdr1->net, ' ', 8)) ) pch[0] = '\0';
		if ( (pch = memchr(phdr1->sta, ' ', 8)) ) pch[0] = '\0';
		if ( (pch = memchr(phdr1->chn, ' ', 8)) ) pch[0] = '\0';
		if ( (pch = memchr(phdr1->loc, ' ', 8)) ) pch[0] = '\0';
		if ( !strncmp(phdr1->loc, SAC_CHAR_UNDEFINED, 6) ) phdr1->loc[0] = '\0';
		
		memset(&tm, 0, sizeof(tm));
		tm.tm_year  = phdr1->year-1900;
		tm.tm_mon   = 0;
		tm.tm_mday  = phdr1->yday;
		tm.tm_hour  = phdr1->hour;
		tm.tm_min   = phdr1->min;
		tm.tm_sec   = phdr1->sec;
		tm.tm_isdst = 0;
		/* phdr1->t    = utc_mktime(&tm); */
		phdr1->t    = my_timegm(&tm);
		
		if (nerr) {
			printf ("ReadManySacs: ERROR reading the %s file (rsac1, nerr=%d)\n", filename, nerr);
			return -2;
		}

		if (npts < N) {
			printf ("ReadManySacs: Files having too short sequences are not supported yet (%d:%d, %s)\n", N, npts, filename);
			printf ("ReadManySacs: Skipping trace %u\n", tr);
			nskip += 1;
			continue;
		} else if (npts > N) {
			printf ("ReadManySacs: WARNING: Files having too long sequences are cut (%d:%d, %s)\n", N, npts, filename);
			phdr1->npts = N;
		}
		
		if (fabs(dt-dt1) > dt1*0.001) {
			printf ("ReadManySacs: Different sampling rate!!! (%f:%f, %s)\n", dt1, dt, filename);
			printf ("ReadManySacs: Skipping trace %u\n", tr);
			nskip += 1;
			continue;
		}
		if (fabs(beg1-beg) > dt1) {
			printf ("ReadManySacs: Different beginning time!!! (%f:%f, %s)\n", beg1, beg, filename);
			printf ("ReadManySacs: Skipping trace %u\n", tr);
			nskip += 1;
			continue;
		}
		
		/* Copy the data */
		if (x != NULL && sig != NULL) memcpy(x[tr-nskip], sig, N*sizeof(float));
	}
	Tr -= nskip;
	
	/* Clean up */
	if (x != NULL) {
		for (tr=Tr; tr<Tr+nskip; tr++) {
			fftw_free(x[tr]);
			x[tr] = NULL;
		}
	}
	if (sig != NULL) free(sig);
	if (filenamesOut != NULL) *filenamesOut = filenames;
	else DestroyFilelist(filenames);
	
	*TrOut = Tr;
	*NOut  = N;
	*dtOut = dt1;
	if (xOut != NULL) *xOut  = x;
	*SacHeaderOut = SacHeader;
	
	return 0;
}

int ReadManySacs_WithDiffLength (float **xOut[], t_HeaderInfo *SacHeaderOut[], unsigned int *TrOut, char *fin) {
	t_HeaderInfo *SacHeader=NULL, *phdr1;
	float **x = NULL, *sig=NULL;
	float beg, dt;
	unsigned int tr, Tr, nskip;
	int nerr, Nmax, npts;
	char **filenames = NULL, *filename=NULL, *pch;
	struct tm tm;
	
	*TrOut = 0;
	*xOut  = NULL;
	*SacHeaderOut = NULL;
	
	if (fin == NULL) {printf("ReadManySacs: NULL filename\n"); return -1;}
	
	if ( (nerr = CreateFilelist (&filenames, &Tr, fin)) ) {
		printf("ReadManySacs: cannot read %s file (CreateFileList error = %d)\n", fin, nerr);
		return -2;
	}
	
	if (Tr == 0) {
		printf("ReadManySacs: warning nothing to read for %s.\n", fin);
		DestroyFilelist(filenames);
		return -2;
	}
	
	if (NULL == (SacHeader = (t_HeaderInfo *)calloc(Tr, sizeof(t_HeaderInfo)) )) {
		DestroyFilelist(filenames);
		return 4;
	}
	
	/* Allocate memory for the input traces */
	if (NULL == (x = (float **)calloc(Tr, sizeof(float *)) )) {
		DestroyFilelist(filenames);
		free(SacHeader);
		printf ("ReadManySacs: Out of memory when reading files.\n"); 
		return 4;
	}
	
	/*************************/
	/* Reading and checking. */
	/*************************/
	nskip = 0;
	for (tr=0; tr<Tr; tr++) {
		filename = filenames[tr];
		
		rsach (filename, &nerr,  strlen(filename));       if (nerr) return nerr_print (filename, nerr);
		getnhv ("npts",  &Nmax,   &nerr, strlen("npts")); if (nerr) return nerr_print (filename, nerr);
		
		if (NULL == (x[tr-nskip] = (float *)fftw_malloc(Nmax*sizeof(float)) )) nerr = 4;
		if (NULL == (sig   = (float *)realloc(sig, Nmax*sizeof(float)) ))   nerr = 4;
		if (nerr == 4) {
			DestroyFilelist(filenames);
			Destroy_FloatArrayList(x, Tr);
			free(sig);
			free(SacHeader);
			printf ("ReadManySacs: Out of memory when reading %s (npts = %d)\n", filename, Nmax); 
			return nerr;
		}
		
		rsac1(filename, sig, &npts, &beg, &dt, &Nmax, &nerr, strlen(filename));
		// sac_warning_off (); // Not supported from v102.0
		
		phdr1 = &SacHeader[tr-nskip];
		phdr1->b     = beg;
		phdr1->dt    = dt;
		phdr1->npts  = npts;
		/* Get a few more sac header fields */
		getnhv ("nzyear", &phdr1->year, &nerr, strlen("nzyear"));
		getnhv ("nzjday", &phdr1->yday, &nerr, strlen("nzjday"));
		getnhv ("nzhour", &phdr1->hour, &nerr, strlen("nzhour"));
		getnhv ("nzmin",  &phdr1->min,  &nerr, strlen("nzmin"));
		getnhv ("nzsec",  &phdr1->sec,  &nerr, strlen("nzsec"));
		getnhv ("nzmsec", &phdr1->msec, &nerr, strlen("nzmsec"));
		getkhv ("knetwk",  phdr1->net,  &nerr, strlen("knetwk"), 8); if (nerr) phdr1->net[0] = '\0';
		getkhv ("kstnm",   phdr1->sta,  &nerr, strlen("kstnm"),  8); if (nerr) phdr1->sta[0] = '\0';
		getkhv ("kcmpnm",  phdr1->chn,  &nerr, strlen("kcmpnm"), 8); if (nerr) phdr1->chn[0] = '\0';
		getkhv ("khole",   phdr1->loc,  &nerr, strlen("khole"),  8); if (nerr) phdr1->loc[0] = '\0';
		getfhv ("stla",   &phdr1->stla, &nerr, strlen("stla")); if (nerr) phdr1->nostloc = 1;
		getfhv ("stlo",   &phdr1->stlo, &nerr, strlen("stlo")); if (nerr) phdr1->nostloc = 1;
		getfhv ("stel",   &phdr1->stel, &nerr, strlen("stel")); if (nerr) phdr1->stel = 0;
		getfhv ("stdp",   &phdr1->stdp, &nerr, strlen("stdp")); if (nerr) phdr1->stdp = 0;
		getfhv ("cmpaz",  &phdr1->cmpaz,  &nerr, strlen("cmpaz"));  if (nerr) { phdr1->cmpaz = 0;  phdr1->nocmp = 1; }
		getfhv ("cmpinc", &phdr1->cmpinc, &nerr, strlen("cmpinc")); if (nerr) { phdr1->cmpinc = 0; phdr1->nocmp = 1; }
		if ( (pch = memchr(phdr1->net, ' ', 8)) ) pch[0] = '\0';
		if ( (pch = memchr(phdr1->sta, ' ', 8)) ) pch[0] = '\0';
		if ( (pch = memchr(phdr1->chn, ' ', 8)) ) pch[0] = '\0';
		if ( (pch = memchr(phdr1->loc, ' ', 8)) ) pch[0] = '\0';
		if ( !strncmp(phdr1->loc, SAC_CHAR_UNDEFINED, 6) ) phdr1->loc[0] = '\0';
		
		if (nerr) {
			printf("ReadManySacs: Error reading %s file (rsac1, nerr=%d)\n", filename, nerr);
			DestroyFilelist(filenames);
			Destroy_FloatArrayList(x, Tr);
			free(sig);
			free(SacHeader);
			return -2;
		}
		if (fabs(dt - SacHeader->dt) > dt*0.001) {
			printf ("ReadManySacs: Different sampling rate!!! (%f:%f, %s)\n", SacHeader->dt, dt, filename);
			printf ("ReadManySacs: Skipping trace %u\n", tr);
			free(x[tr-nskip]);
			nskip += 1;
			continue;
		}
		
		memset(&tm, 0, sizeof(tm));
		tm.tm_year  = phdr1->year-1900;
		tm.tm_mon   = 0;
		tm.tm_mday  = phdr1->yday;
		tm.tm_hour  = phdr1->hour;
		tm.tm_min   = phdr1->min;
		tm.tm_sec   = phdr1->sec;
		tm.tm_isdst = 0;
		/* phdr1->t    = utc_mktime(&tm); */
		phdr1->t    = my_timegm(&tm);
		
		/* Copy the data */
		memcpy(x[tr-nskip], sig, npts*sizeof(float));
	}
	Tr -= nskip;
	
	/* Clean up */
	for (tr=Tr; tr<Tr+nskip; tr++) {
		fftw_free(x[tr]); 
		x[tr] = NULL;
	}
	free(sig);
	DestroyFilelist(filenames);
	
	*TrOut = Tr;
	*xOut  = x;
	*SacHeaderOut = SacHeader;
	
	return 0;
}


void DestroyFilelist(char *p[]) {
	if (p) {
		if (p[0]) free(p[0]);
		free(p);
	}
}

int CreateFilelist (char **filename[], unsigned int *Tr, char *filelist) {
	long pos;
	unsigned int n, N, er=0;
	char *str0, *str1, **files, *mem_filenames;
	FILE *fid;
	
	*Tr = 0;
	*filename = NULL;
	
	/* Test parameters. */
	if (!filelist) return 1;
	
	/* Open the file and check its size. */
	if (NULL == (fid = fopen(filelist, "r"))) 
		{ printf("CreateFilelist: cannot open %s file\n", filelist); return 3; }
	
	/* Allocated memory to read file names. */
	if (NULL == (str0 = (char *)malloc(1024*sizeof(char)) )) 
		{ printf("CreateFilelist: out of memory."); return 2; }
	
	/* Get the number of lines & filesize */
	for (N=0; fgets(str0, 1024, fid); N++);
	fseek(fid, 0L, SEEK_END); /* Not actually needed. */
	pos=ftell(fid);
	fseek(fid, 0L, SEEK_SET);
	
	/* Allocate memory */
	files = (char **)calloc(N, sizeof(char *));
	mem_filenames = (char *)malloc((pos + N)*sizeof(char *));
	*filename = files;
	
	/* Read the file */
	if (files == NULL || mem_filenames == NULL) {
		er = 2;
		printf("CreateFilelist: out of memory.");
		free(mem_filenames);
		free(files);
		*Tr = 0;
	} else {
		pos = 0;
		for (n=0; n<N; n++) {
			if (NULL == fgets(str0, 1024, fid)) break;
			str1 = strchr(str0,'\n');        /* Find newline character                 */
			if (str1) *str1 = '\0';          /* to properly set the end of the string. */
			
			mem_filenames[pos] = '\0';     /* Needed on the strcat below if no folder. */
			files[n] = &mem_filenames[pos];
			strcat(files[n], str0);        /* Add the filename.                        */
			pos += strlen(files[n]) + 1;   /* Update position at the filenames memory. */
		}
		*Tr = N;
	}
	fclose(fid);
	
	free(str0);
	return er;
}

int RemoveZeroTraces (float **xOut[], t_HeaderInfo *SacHeader[], unsigned int *pTr, unsigned int N) {
	unsigned int tr, n, nskip;
	int ia1;
	float **x = *xOut, *px;
	t_HeaderInfo *phd = *SacHeader;
	
	if (xOut == NULL || SacHeader == NULL || pTr == NULL) {
		printf("RemoveZeroTraces: One required variable is NULL.\n");
		return 1;
	}
	nskip = 0;
	for (tr=0; tr<*pTr; tr++) {
		ia1 = 0;
		px = x[tr];
		for (n=0; n<N; n++)
			if (0 != px[n]) { ia1 = 1; break; }
		if (ia1 == 0) {
			nskip++;
			fftw_free(x[tr]); x[tr] = NULL;
			printf("RemoveZeroTraces: Skipping %s.%s.%s.%s at %4d-%03d %02d:%02d:%02d, it's all zeros!\n", 
				phd[tr].net, phd[tr].sta, phd[tr].loc, phd[tr].chn, phd[tr].year, phd[tr].yday, 
				phd[tr].hour, phd[tr].min, phd[tr].sec);
		} else if (nskip) {
			memcpy (&phd[tr-nskip], &phd[tr], sizeof(t_HeaderInfo));
			x[tr-nskip] = x[tr];
		}
	}
	*pTr -= nskip;
	
	return 0;
}

int Read_ManySacsFile (float **xOut[], t_HeaderInfo *SacHeaderOut[], unsigned int *TrOut, unsigned int *NOut, float *dtOut, char *infile) {
	t_HeaderManySacsBinary mhdr;
	t_HeaderInfo *SacHeader = NULL;
	float **x = NULL;
	unsigned int tr, Tr, N, npts;
	size_t nitems;
	int nerr = 0;
	FILE *fid;
	
	if (NULL == (fid = fopen(infile, "r"))) {
		printf("ReadManySacsFile: cannot open %s file\n", infile);
		return -2;
	}
	
	nitems = fread(&mhdr, sizeof(t_HeaderManySacsBinary), 1, fid);
	if (nitems != 1) {
		printf("ReadManySacsFile: cannot read %s file.\n", infile);
		return -2;
	}
	
	if (mhdr.npts == 0) {
		printf("ReadManySacsFile: %s has traces with different lengths, to be eventually done.\n", infile);
		return -2;
	}
	if (strcmp(mhdr.FormatID, "MSACS1")) {
		printf("ReadManySacsFile: %s is not in MSACS1 format.\n", infile);
		return -2;
	}
	Tr = mhdr.nseq;
	N  = mhdr.npts;
	
	if (NULL == (SacHeader = (t_HeaderInfo *)calloc(Tr, sizeof(t_HeaderInfo)) )) nerr = 4;
	if (NULL == (x = Create_FloatArrayList (N, Tr) )) nerr = 4;
	
	if (nerr == 4) {
		printf("ReadManySacsFile: Out of memory when reading %s.\n", infile);
	} else {
		for (tr=0; tr<Tr; tr++) {
			nitems = fread(&SacHeader[tr], sizeof(t_HeaderInfo), 1, fid);
			if (nitems != 1) { 
				printf("ReadManySacsFile: Unexpected end of %s.\n", infile);
				nerr = 5; break;
			}
			npts = SacHeader[tr].npts;
			if (N < npts) npts = N;
			nitems = fread(x[tr], sizeof(float), npts, fid);
			if (nitems != npts) { 
				printf("ReadManySacsFile: Unexpected end of %s.\n", infile);
				nerr = 5; break;
			}
		}
	}
	
	if (nerr) {
		free(SacHeader);
		Destroy_FloatArrayList (x, Tr);
	}
	
	fclose(fid);
	
	*TrOut = Tr;
	*NOut  = N;
	*dtOut = SacHeader[0].dt;
	*xOut  = x;
	*SacHeaderOut = SacHeader;
	
	return nerr;
}

int Write_ManySacsFile (float *x[], t_HeaderInfo *hdr, unsigned int Tr, unsigned int N, char *outfile) {
	t_HeaderManySacsBinary mhdr = {"MSACS1", Tr, N, 1};
	unsigned int tr;
	FILE *fid;
	
	if (NULL == (fid = fopen(outfile, "w"))) {
		printf("WriteManySacsFile: cannot create %s file\n", outfile);
		return -2;
	}
	
	fwrite(&mhdr, sizeof(t_HeaderManySacsBinary), 1, fid);
	for (tr=0; tr<Tr; tr++) {
		fwrite(&hdr[tr], sizeof(t_HeaderInfo), 1, fid);
		fwrite(x[tr], sizeof(float), N, fid);
	}
	
	fclose(fid);
	
	return 0;
}

int ReadLocation_ManySacsFile (double *stlat, double *stlon, char *infile) {
	t_HeaderManySacsBinary mhdr;
	t_HeaderInfo SacHeader;
	size_t nitems;
	int nerr = 0;
	FILE *fid;
	
	if (NULL == (fid = fopen(infile, "r"))) {
		printf("ReadLocation_ManySacsFile: cannot open %s file\n", infile);
		return -2;
	}
	
	nitems = fread(&mhdr, sizeof(t_HeaderManySacsBinary), 1, fid);
	if (nitems != 1) {
		printf("ReadLocation_ManySacsFile: cannot read %s file\n", infile);
		return -2;
	}
	
	if (strcmp(mhdr.FormatID, "MSACS1")) {
		printf("ReadLocation_ManySacsFile: %s is not in MSACS1 format.\n", infile);
		return -2;
	}
	
	if (mhdr.nseq > 1) {
		nitems = fread(&SacHeader, sizeof(t_HeaderInfo), 1, fid);
		if (nitems != 1) { 
			printf("ReadLocation_ManySacsFile: Unexpected end of %s.\n", infile);
			nerr = 5;
		} else {
			*stlat = SacHeader.stla;
			*stlon = SacHeader.stlo;
		}
	}
	
	fclose(fid);
	
	return nerr;
}
