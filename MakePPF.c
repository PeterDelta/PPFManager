// written by Icarus/Paradox improved by PeterDelta
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
#include <stdint.h>
#include "ppfmanager.h"
extern volatile LONG g_app_closing;
extern volatile LONG g_cancel_requested;
#ifndef BUILD_STANDALONE
#endif 
#ifdef BUILD_STANDALONE
#endif
#ifdef BUILD_STANDALONE
#endif

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
	int  blockcheck;
	int  blockcheck_set;
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


int		OpenFilesForCreate(void);
int		PPFCreateHeader(void);
int		PPFGetChanges(void);

long long	WriteChanges(int amount, __int64 chunk);
int		CheckIfPPF3(void);
int		CheckIfFileId(void);
int		PPFAddFileId(void);
void	CloseAllFiles(void);
void	PPFCreatePatch(void);
void	CheckSwitches(int argc, char **argv);
void	PPFShowPatchInfo(void);


static void (*MakePPF_ProgressCallback)(double) = NULL;
void MakePPF_SetProgressCallback(void (*cb)(double)) { MakePPF_ProgressCallback = cb; }

static __int64 MakePPF_TotalBinFileSize = 0;






#ifdef BUILD_STANDALONE

int main(int argc, char **argv)
#else

int MakePPF_Main(int argc, char **argv)
#endif
{
	if (argc < 2) {
		printf("[DEBUG] MakePPF_Main ejecutado, argc=%d\n", argc); fflush(stdout);
		printf("Usage: PPFManager.exe <command> [args]\n");
		printf("Commands: c (create), f (add %s), s (show info)\n", PPFManager_FileIdNameA());
		fflush(stdout);
		return 1;
	}

	
	Arg.undo=0;
	Arg.blockcheck=1;
	Arg.blockcheck_set=0;
	Arg.imagetype=0;
	Arg.fileid=0;
	Arg.desc=0;


	char cmd = argv[1][0];
	int ok = 0;
	if ((cmd == 'c' && argc < 5) || (cmd == 'f' && argc < 4)) {
		printf("Usage: PPF <command> [-<sw> [-<sw>...]] <original bin> <modified bin> <ppf>\n");
		printf("<Commands>\n");
		printf("  c : create PPF3.0 patch            a : add %s\n", PPFManager_FileIdNameA());
		printf("  s : show patchinfomation\n");
		printf("<Switches>\n");
		printf(" -u        : include undo data (default=off)\n");
		printf(" -x        : disable patchvalidation (default=off)\n");
		printf(" -i [0/1]  : imagetype, 0 = BIN, 1 = GI (default=bin)\n");
		printf(" -d \"text\" : use \"text\" as description\n");
		printf(" -f \"file\" : add \"file\" as %s\n", PPFManager_FileIdNameA());
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
							printf("Error: patch already contains %s\n", PPFManager_FileIdNameA());
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
		
		printf("Usage: PPFManager.exe <command> [args]\n");
		printf("Commands: c (create), f (add %s), s (show info)\n", PPFManager_FileIdNameA());
		return 1;
	}
	
	printf("Done.\n");
	return 0;

}


void PPFCreatePatch(void){

	
	if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(0.0);

	if(PPFCreateHeader()){ printf("Error: headercreation failed\n"); if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(100.0); return; }

	if (PPFGetChanges()) { printf("Error: failed while collecting changes or it was canceled\n"); if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(100.0); CloseAllFiles(); return; }
	if(Arg.fileid) { if (PPFAddFileId()) { printf("Error: failed to add %s\n", PPFManager_FileIdNameA()); CloseAllFiles(); return; } }

	
	if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(100.0);

	
	patch_ok = 1;
	return;
}


int PPFCreateHeader(void){
	unsigned char method=0x02, description[128], binblock[1024], dummy=0, i=0;
	unsigned char magic[]="PPF30";

	printf("Writing header... "); fflush(stdout);

	memset(description, 0x20, 50);

	if(Arg.desc){
		size_t desc_len = strlen(Arg.description);
		if(desc_len > 50) desc_len = 50;
		for(i=0;i<desc_len;i++){
			description[i]=Arg.description[i];
		}
		
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
		
		if(Arg.imagetype == 1){
			_lseeki64(bin, 0x80A0, SEEK_SET); 
		} else if(Arg.imagetype == 2) {
			_lseeki64(bin, 0x8000, SEEK_SET); 
			
		} else {
			_lseeki64(bin, 0x9320, SEEK_SET); 
		}
		{
			int _rv = _read(bin, &binblock, 1024);
			if (_rv < 0) { printf("Error: cannot read binblock\n"); return(1); }
			if (_rv < 1024) memset(binblock + _rv, 0, 1024 - _rv);
		}
		if (safe_write(ppf, &binblock, 1024) != 0) return(1);
	}

	
	printf("Done.\n"); fflush(stdout);
	return(0);
}


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






int PPFGetChanges(void){
    unsigned long long __perf_t0 = 0;
    if (perf_enabled()) __perf_t0 = perf_now();
    int read = 0;
    int chunk_size = 8 * 1024 * 1024; 
    long long changes=0;
    unsigned long long found=0;
    __int64 chunk=0, filesize;
    float percent;

	
	a = malloc(chunk_size);
	x = (unsigned char*)(a);
	if (x == NULL) { printf("Error: insufficient memory available\n"); CloseAllFiles(); return(1); }

	
	b = malloc(chunk_size);
	y = (unsigned char*)(b);
	if (y == NULL) { printf("Error: insufficient memory available\n"); free(x); CloseAllFiles(); return(1); }

	filesize = _filelengthi64(bin);
	MakePPF_TotalBinFileSize = filesize; 
	if (filesize == 0) { free(x); free(y); printf("Error: filesize of bin file is zero!\n"); MakePPF_TotalBinFileSize = 0; return(1); }

	_lseeki64(bin,0,SEEK_SET);
	_lseeki64(mod,0,SEEK_SET);

	printf("Finding differences... \n");
	printf("Progress: "); fflush(stdout);
	do{
		
		if (InterlockedCompareExchange(&g_app_closing,0,0) || InterlockedCompareExchange(&g_cancel_requested,0,0)) {
			free(x); free(y);
			return 1; 
		}
		read = (int)((filesize - chunk) > chunk_size ? chunk_size : (int)(filesize - chunk));
		if (read <= 0) break;
		int rr = _read(bin, x, read);
		if (rr < 0) { printf("Error: failed reading from source bin file\n"); free(x); free(y); return(1); }
		if (rr == 0) break; 
		if (_read(mod, y, rr) != rr) { printf("Error: short read from mod file\n"); free(x); free(y); return(1); }
		long long rc = WriteChanges(rr, chunk);
		if (rc < 0) { printf("Error: failed while writing changes\n"); free(x); free(y); return(1); }
		changes = rc;
		read = rr;

		
		chunk += read;
		found += changes;

		percent=(float)chunk/filesize;
		
		if (MakePPF_ProgressCallback) {
			float overall = 1.0f + (percent * 98.0f);
			if (overall < 0.0f) overall = 0.0f;
			if (overall > 99.0f) overall = 99.0f; 
			MakePPF_ProgressCallback((double)overall);
		}

	} while (read!=0);



	if (MakePPF_ProgressCallback) MakePPF_ProgressCallback(100.0);
	
	MakePPF_TotalBinFileSize = 0;
	printf(" 100.00 %% (%llu entries found).\n", (unsigned long long)found);

	
	free(x); free(y);
    if (perf_enabled()) perf_report_ms("PPFGetChanges", __perf_t0, perf_now());
	return(0);
}


long long WriteChanges(int amount, __int64 chunk){
	long long found = 0;
	__int64 i = 0;
	__int64 offset;
	
	int report_step = (int)(amount / 64);
	if (report_step < 1) report_step = 1;    
	if (report_step > 65536) report_step = 65536; 

	for (i = 0; i < (__int64)amount; i++) {
		
		if (MakePPF_ProgressCallback && MakePPF_TotalBinFileSize > 0 && ((i % (__int64)report_step) == 0)) {
			double processed = ((double)(chunk + (__int64)i)) / (double)MakePPF_TotalBinFileSize;
			double overall = 1.0 + (processed * 98.0);
			if (overall < 0.0) overall = 0.0;
			if (overall > 99.0) overall = 99.0;
			MakePPF_ProgressCallback(overall);
		}

		if (x[i] != y[i]) {
			unsigned char k = 0;
			offset = chunk + i;
			do {
				k++; i++;
			} while (i < (__int64)amount && x[i] != y[i] && k != 0xff);
			{
				unsigned char entry[8 + 1 + 255 + 255]; 
				size_t entrylen = 0;
				
				for (int b = 0; b < 8; ++b) entry[entrylen++] = (unsigned char)(((__int64)offset >> (b*8)) & 0xFF);
				
				entry[entrylen++] = k;
				
				memcpy(entry + entrylen, &y[i - k], (size_t)k);
				entrylen += (size_t)k;
				
				if (Arg.undo) { memcpy(entry + entrylen, &x[i - k], (size_t)k); entrylen += (size_t)k; }
				
				if (safe_write(ppf, entry, entrylen) != 0) return -1;
			}
			found++;
			if (k == 0xff) i--;
		}
	}
	return(found);
}


void CheckSwitches(int argc, char **argv){
    int i;
    unsigned char *x;

    for(i=2;i<argc;i++){
        x=(unsigned char*)(argv[i]);
        if(x[0]=='-'){
            char opt = x[1];
            
            if ((opt=='i' || opt=='d' || opt=='f') && i+1 >= argc) { continue; }

            switch(opt){
                case 'u': Arg.undo=1; break;
                case 'x': Arg.blockcheck=0; Arg.blockcheck_set=1; break;
                case 'i': if (i+1 < argc) {
                              if (*argv[i+1]=='0') Arg.imagetype=0;
                              else if (*argv[i+1]=='1') Arg.imagetype=1;
                              else if (*argv[i+1]=='2') Arg.imagetype=2;
                              i++;
                          }
                          break;
                case 'd': if (i+1 < argc) { Arg.desc=1; Arg.description=argv[i+1]; i++; } break;
                case 'f': if (i+1 < argc) { Arg.fileid=1; Arg.fileidname=argv[i+1]; i++; } break;
                default: break;
            }
        }
    }
    
    if (Arg.imagetype == 2 && Arg.blockcheck_set == 0) {
        Arg.blockcheck = 0;
    }
    Arg.ppfname=argv[argc-1];
    Arg.modname=argv[argc-2];
    Arg.origname=argv[argc-3];
}
int OpenFilesForCreate(void){
    
    bin = _open(Arg.origname, _O_RDONLY | _O_BINARY | _O_SEQUENTIAL);
    if (bin == -1) {
        PrintWin32ErrorFmt("Error: cannot open file '%s'", Arg.origname);
        CloseAllFiles();
        return 0;
    }
    mod=_open(Arg.modname,  _O_RDONLY | _O_BINARY | _O_SEQUENTIAL);
	if(mod==-1){
		PrintWin32ErrorFmt("Error: cannot open file '%s'", Arg.modname);
		CloseAllFiles();
		return(0);
	}

	
	if((_filelengthi64(bin)) != (_filelengthi64(mod))){
		printf("Error: input files are different in size.\n");
		CloseAllFiles();
		return(0);
	}
{
        char tmpname[512];
        int tmpfd = -1;
        using_temp = 0;
        
        {
            char dir[MAX_PATH];
            strncpy(dir, Arg.ppfname, sizeof(dir)-1); dir[sizeof(dir)-1] = '\0';
            char *p = strrchr(dir, '\\'); if (!p) p = strrchr(dir, '/');
            if (p) *p = '\0'; else if (!GetTempPathA(MAX_PATH, dir)) { strcpy_s(dir, sizeof(dir), "."); }

            if (GetTempFileNameA(dir, "ppf", 0, tmpname)) {
                
                tmpfd = _open(tmpname, _O_BINARY | _O_RDWR);
                if (tmpfd != -1) {
                    ppf = tmpfd; 
                    strncpy(temp_ppfname, tmpname, sizeof(temp_ppfname)-1);
                    temp_ppfname[sizeof(temp_ppfname)-1] = '\0';
                    using_temp = 1;
                } else {
                    
                    remove(tmpname);
                }
            }
        }
        
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


void CloseAllFiles(void){
	if(ppf>0) _close(ppf);

	if(using_temp){
		if(patch_ok){
			
			if (!MoveFileExA(temp_ppfname, Arg.ppfname, MOVEFILE_REPLACE_EXISTING)) {
				
				DeleteFileA(Arg.ppfname);
				if (!MoveFileExA(temp_ppfname, Arg.ppfname, MOVEFILE_REPLACE_EXISTING)) {
					PrintWin32ErrorFmt("Error: cannot rename temp file '%s' to '%s'", temp_ppfname, Arg.ppfname);
				}
			}
		} else {
			
			remove(temp_ppfname);
		}
		using_temp = 0;
		temp_ppfname[0] = '\0';
	}



	if(bin>0) _close(bin);
	if(mod>0) _close(mod);
	if(fileid>0) _close(fileid);
}










int CheckIfFileId(){
	unsigned char chkmagic[4];

	if (_lseeki64(ppf,-6,SEEK_END) == -1) return 0;
	if (_read(ppf, chkmagic, 4) != 4) return 0;
	
	if(memcmp(chkmagic, ".DIZ", 4) == 0){ return(1); }
	
	return(0);
}




int CheckIfPPF3(){
	unsigned char chkmagic[4];

	_lseeki64(ppf,0,SEEK_SET);
	if (_read(ppf, chkmagic,4) != 4) return(0);
	
	if(memcmp(chkmagic, "PPF3", 4) == 0){ return(1); }
	
	return(0);
}


void PPFShowPatchInfo(void){
	unsigned char x, desc[51];

	int w = PPFManager_LabelWidth();

	printf("Showing patchinfo... \n");

        printf("%-*s : PPF3.0\n", w, "Version");
        printf("%-*s : 2\n", w, "Enc.Method");

	_lseeki64(ppf,56,SEEK_SET);
	if (_read(ppf, &x, 1) != 1) { printf("Error: failed reading patch header (imagetype)\n"); return; }
	printf("%-*s : ", w, "Imagetype");
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
	printf("%-*s : ", w, "Validation");
	if(!x){
		printf("Disabled\n");
	} else {
		printf("Enabled\n");
	}

	_lseeki64(ppf,58,SEEK_SET);
	if (_read(ppf, &x, 1) != 1) { printf("Error: failed reading patch header (undo)\n"); return; }
	printf("%-*s : ", w, "Undo Data");
	if(!x){
		printf("Not available\n");
	} else {
		printf("Available\n");
	}
	
	_lseeki64(ppf,6,SEEK_SET);
	if (_read(ppf, &desc, 50) != 50) { printf("Error: failed reading patch description\n"); return; }
	desc[50]=0;
	PrintDescriptionBytes(desc);

	
	
	
	
	
	
	
	
	
	

	{
	    int w2 = PPFManager_LabelWidth();
	    printf("%-*s : ", w2, PPFManager_FileIdNameA());
	}
	if(!CheckIfFileId()){
		printf("Not available\n");
	} else {
		printf("Available\n");
		unsigned short y;
		if (_lseeki64(ppf, -2, SEEK_END) != -1 && _read(ppf, &y, 2) == 2) {
		    if (y > 3072) y = 3072;
		    unsigned char *id = (unsigned char*)malloc(y + 1);
		    if (id) {
		        if (_lseeki64(ppf, -(y + 18), SEEK_END) != -1 && _read(ppf, id, y) == y) {
		            id[y] = 0;
		            
		            printf("%s", id);
                    if (y > 0 && id[y-1] != '\n') putchar('\n');
		        }
		        free(id);
		    }
		}
	}
}




int PPFAddFileId(void){
	unsigned short fileidlength=0;
	unsigned char fileidstart[]="@BEGIN_FILE_ID.DIZ", fileidend[]="@END_FILE_ID.DIZ", buffer[3072];
	int local_fd = -1; 

	
	if (Arg.fileidname) {
		local_fd = _open(Arg.fileidname, _O_RDONLY | _O_BINARY);
		if (local_fd == -1) {
			PrintWin32ErrorFmt("Error: cannot open %s '%s'", PPFManager_FileIdNameA(), Arg.fileidname);
			return(1);
		}
	}
	int fd = (local_fd != -1) ? local_fd : fileid;
	if (fd == -1) { printf("Error: no file descriptor available for file_id.diz\n"); if (local_fd != -1) _close(local_fd); return(1); }

	if (_lseeki64(fd,0,SEEK_END) == -1) { PrintWin32ErrorFmt("Error: cannot seek end of %s", PPFManager_FileIdNameA()); if (local_fd != -1) _close(local_fd); return(1); }
	fileidlength=(unsigned short)_telli64(fd);
	if(fileidlength>3072) fileidlength=3072;
	_lseeki64(fd,0,SEEK_SET);

	if (_read(fd, &buffer, fileidlength) != fileidlength) {
		int e = errno;
		fprintf(stderr, "Error: failed reading %s '%s' (errno=%d - %s)\n", PPFManager_FileIdNameA(), Arg.fileidname ? Arg.fileidname : "<fd>", e, strerror(e));
		if (local_fd != -1) _close(local_fd);
		return(1);
	}
	_lseeki64(ppf,0,SEEK_END);

	printf("%s %s... ", PPFManager_AddingA(), PPFManager_FileIdNameA()); fflush(stdout);

	if (safe_write(ppf, &fileidstart, 18) != 0) { if (local_fd != -1) _close(local_fd); return(1); }
	if (safe_write(ppf, &buffer, fileidlength) != 0) { if (local_fd != -1) _close(local_fd); return(1); }
	if (safe_write(ppf, &fileidend, 16) != 0) { if (local_fd != -1) _close(local_fd); return(1); }
	if (write_le16(ppf, fileidlength) != 0) { if (local_fd != -1) _close(local_fd); return(1); }
	
	if (local_fd != -1) _close(local_fd);
	printf("Done.\n"); fflush(stdout);
	return(0);
}
