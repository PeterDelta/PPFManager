/*
 *     MakePPF.c
 *     suggestions and some fixes by <Hu Kares>, thanks.
 *
 *     Creates PPF3.0 Patches.
 *     Feel free to use this source in and for your own
 *     programms.
 *
 *     Visual C++ is needed in order to compile this source.
 *---------------------------------------------------------------------
 *	   Code improved by PeterDelta
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <windows.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include "ppfmanager.h"

#ifndef BUILD_STANDALONE
/* Use shared PrintWin32ErrorFmt from PPFManager (no local implementation needed) */
#endif 

#ifdef BUILD_STANDALONE
/* Standalone helpers for I/O/LE are centralized in ppfmanager.h to avoid duplication */
#endif
#ifdef BUILD_STANDALONE
/* Print helpers for standalone builds are provided centrally in ppfmanager.h */
#endif


// Used global variables.
#ifdef BUILD_STANDALONE
int ppf=0, bin=0, mod=0, fileid=0;
char temp_ppfname[512] = {0};
int using_temp = 0;
int patch_ok = 0;
#else
extern int ppf, bin, mod, fileid;
extern char temp_ppfname[512];
extern int using_temp;
extern int patch_ok;
#endif


void *a, *b; 
unsigned char *x, *y;
typedef struct Arg
{
	int  undo;
	int  blockcheck;      /* 1 = validation enabled, 0 = disabled */
	int  blockcheck_set;  /* 1 = user explicitly set validation (via -x), 0 = not specified */
	int  imagetype;
	int  desc;
	int  fileid;
	char *description;
	char *fileidname;
	char *origname;
	char *modname;
	char *ppfname;
}Argumentblock;
Argumentblock Arg;

// Used prototypes.
int		OpenFilesForCreate(void);
int		PPFCreateHeader(void);
int		PPFGetChanges(void);
/* WriteChanges returns number of differences found, or -1 on error. Use 64-bit to avoid overflow on very large files. */
long long	WriteChanges(int amount, __int64 chunk);
int		CheckIfPPF3(void);
int		CheckIfFileId(void);
int		PPFAddFileId(void);
void	CloseAllFiles(void);
void	PPFCreatePatch(void);
void	CheckSwitches(int argc, char **argv);
void	PPFShowPatchInfo(void);


/* Progress callback for GUI integration: percent in range 0.0 .. 100.0 */
static void (*MakePPF_ProgressCallback)(double) = NULL;
void MakePPF_SetProgressCallback(void (*cb)(double)) { MakePPF_ProgressCallback = cb; }

static __int64 MakePPF_TotalBinFileSize = 0;






#ifdef BUILD_STANDALONE
// Main routine (standalone build only).
int main(int argc, char **argv)
#else
// Main routine (integrated build).
int MakePPF_Main(int argc, char **argv)
#endif
{
	if (argc < 2) {
		// No comando: mostrar ayuda
		printf("[DEBUG] MakePPF_Main ejecutado, argc=%d\n", argc); fflush(stdout);
		printf("Usage: PPFManager.exe <command> [args]\n");
		printf("Commands: c (create), f (add file_id.diz), s (show info)\n");
		fflush(stdout);
		return 1;
	}

	// Setting defaults.
	Arg.undo=0;
	Arg.blockcheck=1;
	Arg.blockcheck_set=0;
	Arg.imagetype=0;
	Arg.fileid=0;
	Arg.desc=0;


	char cmd = argv[1][0];
	int ok = 0;
	// Imprimir cabecera MakePPF clásico para 'c' y 'f' solo si faltan argumentos
	if ((cmd == 'c' && argc < 5) || (cmd == 'f' && argc < 4)) {
		printf("Usage: PPF <command> [-<sw> [-<sw>...]] <original bin> <modified bin> <ppf>\n");
		printf("<Commands>\n");
		printf("  c : create PPF3.0 patch            a : add file_id.diz\n");
		printf("  s : show patchinfomation\n");
		printf("<Switches>\n");
		printf(" -u        : include undo data (default=off)\n");
		printf(" -x        : disable patchvalidation (default=off)\n");
		printf(" -i [0/1]  : imagetype, 0 = BIN, 1 = GI (default=bin)\n");
		printf(" -d \"text\" : use \"text\" as description\n");
		printf(" -f \"file\" : add \"file\" as file_id.diz\n");
		printf("\nExamples: PPF c -u -i 1 -d \"my elite patch\" game.bin patch.bin output.ppf\n");
		printf("          PPF a patch.ppf myfileid.txt\n\n");
	}
	switch (cmd) {
		case 'c':
			if (argc >= 5) {
				CheckSwitches(argc, argv);
				if (OpenFilesForCreate()) {
					PPFCreatePatch();
					CloseAllFiles();
					ok = 1;

				}
			}
			break;
		case 's':
			if (argc >= 3) {
				ppf = _open(argv[2], _O_RDONLY | _O_BINARY);
				if (ppf != -1 && CheckIfPPF3()) {
					PPFShowPatchInfo();
					_close(ppf);
					ok = 1;
				} else if (ppf != -1) {
					printf("Error: file '%s' is no PPF3.0 patch\n", argv[2]);
					_close(ppf);
				}
			}
			break;
		case 'f':
			if (argc >= 4) {
				ppf = _open(argv[2], _O_BINARY | _O_RDWR);
				if (ppf != -1) {
					fileid = _open(argv[3], _O_RDONLY | _O_BINARY);
					if (fileid != -1 && CheckIfPPF3()) {
						if (!CheckIfFileId()) {
							PPFAddFileId();
						} else {
							printf("Error: patch already contains a file_id.diz\n");
						}
						CloseAllFiles();
						ok = 1;
					} else if (fileid != -1) {
						printf("Error: file '%s' is no PPF3.0 patch\n", argv[2]);
						CloseAllFiles();
					}
				}
			}
			break;
		default:
			printf("Error: unknown command '%s'\n", argv[1]);
			break;
	}

	if (!ok) {
		// Solo mostrar ayuda si no se ejecutó ningún comando válido
		printf("Usage: PPFManager.exe <command> [args]\n");
		printf("Commands: c (create), f (add file_id.diz), s (show info)\n");
		return 1;
	}
	/* Canonical final message (always the same to avoid cosmetic diffs) */
	printf("Done.\n");
	return 0;

}

// Start to create the patch.
void PPFCreatePatch(void){

	/* Start progress */
	if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(0.0);

	if(PPFCreateHeader()){ printf("Error: headercreation failed\n"); if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(100.0); return; }

	if (PPFGetChanges()) { printf("Error: failed while collecting changes\n"); if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(100.0); CloseAllFiles(); return; }
	if(Arg.fileid) { if (PPFAddFileId()) { printf("Error: failed to add file_id.diz\n"); CloseAllFiles(); return; } }

	/* Finish progress */
	if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(100.0);

	/* Mark patch as successfully created so temp file will be renamed */
	patch_ok = 1;
	return;
}

// Create PPF3.0 Header.
// Return: 1 - Failed
// Return: 0 - Success
int PPFCreateHeader(void){
	unsigned char method=0x02, description[128], binblock[1024], dummy=0, i=0;
	unsigned char magic[]="PPF30";

	printf("Writing header... "); fflush(stdout);


	if(Arg.desc){
		size_t desc_len = strlen(Arg.description);
		if(desc_len > 50) desc_len = 50;
		for(i=0;i<desc_len;i++){
			description[i]=Arg.description[i];
		}
		/* Ensure trailing bytes are explicitly spaces (defensive-initialization) */
		for (size_t _j = desc_len; _j < 50; ++_j) description[_j] = 0x20;
	}

	if(safe_write(ppf, &magic, 5) != 0) { printf("Error: failed to write header\n"); return(1); }
	if(safe_write(ppf, &method, 1) != 0) { printf("Error: failed to write header\n"); return(1); }
	if(safe_write(ppf, &description, 50) != 0) { printf("Error: failed to write header\n"); return(1); }
	{
		unsigned char itype = (unsigned char)Arg.imagetype;
		if (safe_write(ppf, &itype, 1) != 0) { printf("Error: failed to write header\n"); return(1); }
	}
	{
		unsigned char bcheck = (unsigned char)Arg.blockcheck;
		if (safe_write(ppf, &bcheck, 1) != 0) { printf("Error: failed to write header\n"); return(1); }
	}
	{
		unsigned char udata = (unsigned char)Arg.undo;
		if (safe_write(ppf, &udata, 1) != 0) { printf("Error: failed to write header\n"); return(1); }
	}
	if(safe_write(ppf, &dummy, 1) != 0) { printf("Error: failed to write header\n"); return(1); }

	if(Arg.blockcheck){
		/* imagetype: 0=BIN, 1=GI, 2=ISO */
		if(Arg.imagetype == 1){
			_lseeki64(bin, 0x80A0, SEEK_SET); /* GI */
		} else if(Arg.imagetype == 2) {
			_lseeki64(bin, 0x8000, SEEK_SET); /* ISO9660 Primary Volume Descriptor at sector 16*2048 */
			/* Validation is disabled for ISO by default (user can enable if desired) */
		} else {
			_lseeki64(bin, 0x9320, SEEK_SET); /* BIN */
		}
		{
			int _rv = _read(bin, &binblock, 1024);
			if (_rv < 0) { printf("Error: cannot read binblock\n"); return(1); }
			if (_rv < 1024) memset(binblock + _rv, 0, 1024 - _rv);
		}
		if (safe_write(ppf, &binblock, 1024) != 0) return(1);
	}

	/* Header creation completed */
	printf("Done.\n"); fflush(stdout);
	return(0);
}

/* Initialize MakePPF global arguments to defaults to allow in-process invocation */
void MakePPF_InitArgs(void) {
	Arg.undo = 0;
	Arg.blockcheck = 1;
	Arg.blockcheck_set = 0;
	Arg.imagetype = 0;
	Arg.fileid = 0;
	Arg.desc = 0;
	Arg.description = NULL;
	Arg.fileidname = NULL;
	Arg.origname = NULL;
	Arg.modname = NULL;
	Arg.ppfname = NULL;
}

// Part of the PPF3.0 algorithm to find file-changes.
// Uses a chunk buffer for scanning; current chunk size is 1 MiB (1048576).
// Two buffers are allocated (total 2 MiB). Adjust the constant if larger
// chunks are desired or required by the platform.
// Return: 1 - Failed
// Return: 0 - Success
int PPFGetChanges(void){
	int read=0, eightmb=1048576;
	long long changes=0;
	unsigned long long found=0;
	__int64 chunk=0, filesize;
	float percent;

	//Allocate memory (8 Megabit = 1 Chunk)
	a=malloc(eightmb);
	x=(unsigned char*)(a);
	if(x==NULL){ printf("Error: insufficient memory available\n"); CloseAllFiles(); return(1); }

	//Allocate memory (8 Megabit = 1 Chunk)	
	b=malloc(eightmb);
	y=(unsigned char*)(b);
	if(y==NULL){ printf("Error: insufficient memory available\n"); free(x); CloseAllFiles(); return(1); }

	filesize=_filelengthi64(bin);
	MakePPF_TotalBinFileSize = filesize; /* allow WriteChanges() to compute sub-chunk progress */
	if(filesize == 0){ printf("Error: filesize of bin file is zero!\n"); free(x); free(y); MakePPF_TotalBinFileSize = 0; return(1);}

	_lseeki64(bin,0,SEEK_SET);
	_lseeki64(mod,0,SEEK_SET);

	printf("Finding differences... \n");
	printf("Progress: "); fflush(stdout);
	do{
		read = _read(bin, x, eightmb);
		if (read < 0) { printf("Error: failed reading from source bin file\n"); free(x); free(y); return(1); }
		if (read == 0) break; /* EOF */
		if (read != 0) {
			if (read == eightmb) {
				if (_read(mod, y, eightmb) != eightmb) { printf("Error: short read from mod file\n"); free(x); free(y); return(1); }
				long long rc = WriteChanges(eightmb, chunk);
				if (rc < 0) { printf("Error: failed while writing changes\n"); free(x); free(y); return(1); }
				changes = rc;
			} else {
				if (_read(mod, y, read) != read) { printf("Error: short read from mod file\n"); free(x); free(y); return(1); }
				long long rc = WriteChanges(read, chunk);
				if (rc < 0) { printf("Error: failed while writing changes\n"); free(x); free(y); return(1); }
				changes = rc;
			}
		}

		/* Advance by the actual bytes read and update found-count */
		if (read > 0) {
			chunk += read;
			found += changes;
		}

		percent=(float)chunk/filesize;
		/* Report progress early and often to GUI: reserve 1% for header and 1% for finalization; scanning maps to 1..99% */
		if (MakePPF_ProgressCallback) {
			float overall = 1.0f + (percent * 98.0f);
			if (overall < 0.0f) overall = 0.0f;
			if (overall > 99.0f) overall = 99.0f; /* keep finalization for last step */
			MakePPF_ProgressCallback((double)overall);
		}

	} while (read!=0);

	if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(100.0);
	/* Done — clear the filesize sentinel */
	MakePPF_TotalBinFileSize = 0;
	printf(" 100.00 %% (%llu entries found).\n", (unsigned long long)found);

	//Free memory.
	free(x); free(y);
	return(0);
}

// This function actually scans the 8 Mbit blocks and writes down the patchdata
// Return: Found differences.
long long WriteChanges(int amount, __int64 chunk){
	long long found = 0;
	__int64 i = 0;
	__int64 offset;
	/* choose reporting granularity based on chunk size; aim for ~64 steps per chunk but allow small chunks to report frequently */
	int report_step = (int)(amount / 64);
	if (report_step < 1) report_step = 1;    /* ensure at least one update per byte for tiny chunks */
	if (report_step > 65536) report_step = 65536; /* cap frequency for extremely large chunks */

	for (i = 0; i < (__int64)amount; i++) {
		/* Periodic sub-chunk progress report (non-blocking, worker thread) */
		if (MakePPF_ProgressCallback && MakePPF_TotalBinFileSize > 0 && ((i % (__int64)report_step) == 0)) {
			double processed = ((double)(chunk + (__int64)i)) / (double)MakePPF_TotalBinFileSize;
			double overall = 1.0 + (processed * 98.0);
			if (overall < 0.0) overall = 0.0;
			if (overall > 99.0) overall = 99.0; /* reserve last percent for finalization */
			MakePPF_ProgressCallback(overall);
		}

		if (x[i] != y[i]) {
			unsigned char k = 0;
			offset = chunk + i;
			if (write_le64(ppf, (unsigned long long)offset) != 0) return -1;
			do {
				k++; i++;
			} while (i < (__int64)amount && x[i] != y[i] && k != 0xff);
			if (safe_write(ppf, &k, 1) != 0) return -1;
			if (safe_write(ppf, &y[i-k], (size_t)k) != 0) return -1;
			found++;
			if (Arg.undo) {
				if (safe_write(ppf, &x[i-k], (size_t)k) != 0) return -1; // Write undo data as well
			}
			if (k == 0xff) i--;
		}
	}
	return(found);
}

// Check all switches given in commandline and fill Arg structure.
// Return: 0 - Failed
// Return: 1 - Success
int OpenFilesForCreate(void){

	bin=_open(Arg.origname, _O_RDONLY | _O_BINARY | _O_SEQUENTIAL);
	if(bin==-1){
		PrintWin32ErrorFmt("Error: cannot open file '%s'", Arg.origname);
		CloseAllFiles();
		return(0);
	}	
	mod=_open(Arg.modname,  _O_RDONLY | _O_BINARY | _O_SEQUENTIAL);
	if(mod==-1){
		PrintWin32ErrorFmt("Error: cannot open file '%s'", Arg.modname);
		CloseAllFiles();
		return(0);
	}

	//Check if files have same size.
	if((_filelengthi64(bin)) != (_filelengthi64(mod))){
		printf("Error: input files are different in size.\n");
		CloseAllFiles();
		return(0);
	}

	if(Arg.fileid){
		fileid=_open(Arg.fileidname, _O_RDONLY | _O_BINARY);
		if(fileid==-1){
			PrintWin32ErrorFmt("Error: cannot open file '%s'", Arg.fileidname);
			CloseAllFiles();
			return(0);
		}
	}

{
        char tmpname[512];
        int tmpfd = -1;
        using_temp = 0;
        /* Prefer to create a temp file in the same directory as the target via GetTempFileNameA */
        {
            char dir[MAX_PATH];
            strncpy(dir, Arg.ppfname, sizeof(dir)-1); dir[sizeof(dir)-1] = '\0';
            char *p = strrchr(dir, '\\'); if (!p) p = strrchr(dir, '/');
            if (p) *p = '\0'; else if (!GetTempPathA(MAX_PATH, dir)) { strcpy_s(dir, sizeof(dir), "."); }

            if (GetTempFileNameA(dir, "ppf", 0, tmpname)) {
                /* GetTempFileName creates the file; open it for read/write */
                tmpfd = _open(tmpname, _O_BINARY | _O_RDWR);
                if (tmpfd != -1) {
                    ppf = tmpfd; /* reuse descriptor */
                    strncpy(temp_ppfname, tmpname, sizeof(temp_ppfname)-1);
                    temp_ppfname[sizeof(temp_ppfname)-1] = '\0';
                    using_temp = 1;
                } else {
                    /* fallback to original loop below */
                    remove(tmpname);
                }
            }
        }
        /* Fallback: try creating a named temp next to target using exclusive create loop */
        if (!using_temp) {
            int tryi;
            for(tryi=0; tryi<1000; tryi++){
                if(tryi==0) _snprintf_s(tmpname, sizeof(tmpname), _TRUNCATE, "%s.tmp", Arg.ppfname);
                else _snprintf_s(tmpname, sizeof(tmpname), _TRUNCATE, "%s.tmp%03d", Arg.ppfname, tryi);
                ppf = _open(tmpname, _O_BINARY | _O_CREAT | _O_EXCL | _O_RDWR, _S_IREAD | _S_IWRITE);
                if(ppf!=-1){
                    strncpy(temp_ppfname, tmpname, sizeof(temp_ppfname)-1);
                    temp_ppfname[sizeof(temp_ppfname)-1] = '\0';
                    using_temp = 1;
                    break;
                } else {
                    if(errno==EEXIST) continue;
                    else break;
                }
            }
        }
        if(ppf==-1){
            PrintWin32ErrorFmt("Error: cannot create temp file for '%s'", Arg.ppfname);
            CloseAllFiles();
            return(0);
        }
	}


	return(1);
}

// Closing all files which are currently opened.
void CloseAllFiles(void){
	if(ppf>0) _close(ppf);

	/* If we used a temporary file, either rename it to final name on success
	   or remove it on failure/abort. */
	if(using_temp){
		if(patch_ok){
			/* Prefer atomic replacement via MoveFileEx on Windows */
			if (!MoveFileExA(temp_ppfname, Arg.ppfname, MOVEFILE_REPLACE_EXISTING)) {
				/* Try to remove existing destination and retry */
				DeleteFileA(Arg.ppfname);
				if (!MoveFileExA(temp_ppfname, Arg.ppfname, MOVEFILE_REPLACE_EXISTING)) {
					PrintWin32ErrorFmt("Error: cannot rename temp file '%s' to '%s'", temp_ppfname, Arg.ppfname);
				}
			}
		} else {
			/* Cleanup temp file if patch failed or was aborted */
			remove(temp_ppfname);
		}
		using_temp = 0;
		temp_ppfname[0] = '\0';
	}

	if(bin>0) _close(bin);
	if(mod>0) _close(mod);
	if(fileid>0) _close(fileid);
}

// Check if a file_id.diz is available.
// Return: 0 - No file_id.diz
// Return: 1 - Yes.
int CheckIfFileId(){
	unsigned char chkmagic[4];

	if (_lseeki64(ppf,-6,SEEK_END) == -1) return 0;
	if (_read(ppf, chkmagic, 4) != 4) return 0;
	/* The last four bytes of the END marker are ".DIZ" ('.','D','I','Z'). */
	if(memcmp(chkmagic, ".DIZ", 4) == 0){ return(1); }
	
	return(0);
}

// Check if a file is a PPF3.0 Patch.
// Return: 0 - No PPF3.0
// Return: 1 - PPF3.0
int CheckIfPPF3(){
	unsigned char chkmagic[4];

	_lseeki64(ppf,0,SEEK_SET);
	if (_read(ppf, chkmagic,4) != 4) return(0);
	/* The file header starts with "PPF3" (ASCII order). */
	if(memcmp(chkmagic, "PPF3", 4) == 0){ return(1); }
	
	return(0);
}

// Show various patch information. (PPF3.0 Only)
void PPFShowPatchInfo(void){
	unsigned char x, desc[51], id[3072];
	unsigned short y;

	printf("Showing patchinfo... \n");
	// Versión (ejemplo de campo de texto fijo, si algún día se lee de archivo, filtrar)
	printf("Version     : PPF3.0\n");
	printf("Enc.Method  : 2\n");
	_lseeki64(ppf,56,SEEK_SET);
	if (_read(ppf, &x, 1) != 1) { printf("Error: failed reading patch header (imagetype)\n"); return; }
	printf("Imagetype   : ");
	if(x == 0){
		printf("BIN\n");
	} else if(x == 1){
		printf("GI\n");
	} else if(x == 2){
		printf("ISO\n");
	} else {
		printf("Unknown (%d)\n", x);
	}

	_lseeki64(ppf,57,SEEK_SET);
	if (_read(ppf, &x, 1) != 1) { printf("Error: failed reading patch header (validation)\n"); return; }
	printf("Validation  : ");
	if(!x){
		printf("Disabled\n");
	} else {
		printf("Enabled\n");
	}

	_lseeki64(ppf,58,SEEK_SET);
	if (_read(ppf, &x, 1) != 1) { printf("Error: failed reading patch header (undo)\n"); return; }
	printf("Undo Data   : ");
	if(!x){
		printf("Not available\n");
	} else {
		printf("Available\n");
	}
	
	_lseeki64(ppf,6,SEEK_SET);
	if (_read(ppf, &desc, 50) != 50) { printf("Error: failed reading patch description\n"); return; }
	desc[50]=0;
	PrintDescriptionBytes(desc);

	// Si hay otros campos de texto en el futuro, ejemplo de filtro seguro:
	// _lseeki64(ppf, offset, SEEK_SET);
	// if (_read(ppf, fieldbuf, len) == len) {
	//     fieldbuf[len-1]=0;
	//     // Solo imprimir bytes imprimibles y hasta el primer nulo
	//     int i=0;
	//     printf("CampoX      : ");
	//     while (i<len && fieldbuf[i] && fieldbuf[i]>=32 && fieldbuf[i]<127) { putchar(fieldbuf[i]); i++; }
	//     putchar('\n');
	// }

	printf("File_id.diz : ");
	if(!CheckIfFileId()){
		printf("Not available\n");
	} else {
		printf("Available\n");
		_lseeki64(ppf,-2,SEEK_END);
		if (read_le16(ppf, &y) != 0) { printf("Error: failed reading file_id length\n"); return; }
       if (y > 3072) {
			y = 3072;
       }
		_lseeki64(ppf,-(y+18),SEEK_END);
		if (_read(ppf, &id, y) != y) { printf("Error: failed reading file_id.diz\n"); return; }
		id[y]=0;
		/* Print file_id.diz content in console-aware way */
		PrintRawTextBytes(id);
	}	
}

// This routine adds a file_id.diz to a PPF3.0 patch.
// Return: 0 - Okay
// Return: 1 - Failed.
int PPFAddFileId(void){
	unsigned short fileidlength=0;
	unsigned char fileidstart[]="@BEGIN_FILE_ID.DIZ", fileidend[]="@END_FILE_ID.DIZ", buffer[3072];

	_lseeki64(fileid,0,SEEK_END);
	fileidlength=(unsigned short)_telli64(fileid);
	if(fileidlength>3072) fileidlength=3072;
	_lseeki64(fileid,0,SEEK_SET);

	if (_read(fileid, &buffer, fileidlength) != fileidlength) { printf("Error: failed reading file_id.diz\n"); return(1); }
	_lseeki64(ppf,0,SEEK_END);

	printf("Adding file_id.diz... "); fflush(stdout);

	if (safe_write(ppf, &fileidstart, 18) != 0) return(1);
	if (safe_write(ppf, &buffer, fileidlength) != 0) return(1);
	if (safe_write(ppf, &fileidend, 16) != 0) return(1);
	if (write_le16(ppf, fileidlength) != 0) return(1);
	
	printf("Done.\n"); fflush(stdout);
	return(0);
}

// Check all switches given in commandline and fill Arg structure.
void CheckSwitches(int argc, char **argv){
	int i;
	unsigned char *x;

	for(i=2;i<argc;i++){
		x=(unsigned char*)(argv[i]);
		if(x[0]=='-'){
		
			switch(x[1]){
				case 'u'	:	Arg.undo=1; break;
			case 'x'	: 	Arg.blockcheck=0; Arg.blockcheck_set=1; break;
				case 'i'	:	if(*argv[i+1]=='0') Arg.imagetype=0;
						else if(*argv[i+1]=='1') Arg.imagetype=1;
						else if(*argv[i+1]=='2') Arg.imagetype=2;
								i++;
								break;
				case 'd'	:	Arg.desc=1; Arg.description=argv[i+1];
								i++;
								break;
				case 'f'	:	Arg.fileid=1; Arg.fileidname=argv[i+1];
								i++;
								break;
				default		:	break;
			}

		}
	}
	/* If imagetype is ISO and user did not explicitly set validation (-x), disable validation by default. */
	if (Arg.imagetype == 2 && Arg.blockcheck_set == 0) {
		Arg.blockcheck = 0;
	}
	Arg.ppfname=argv[argc-1];
	Arg.modname=argv[argc-2];
	Arg.origname=argv[argc-3];
}
