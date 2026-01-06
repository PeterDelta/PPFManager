// written by Icarus/Paradox improved by PeterDelta
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <windows.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include "ppfmanager.h"
extern volatile LONG g_app_closing;
extern volatile LONG g_cancel_requested;
#ifndef BUILD_STANDALONE
#endif

static int bin_is_temp = 0;
static char bin_orig_name[512] = {0};
static char bin_tmp_name[512] = {0};
static int apply_success = 0; 
int ApplyPPF_GetSuccess(void) { return apply_success; }

static int apply_safe_patch = 0;
#ifdef BUILD_STANDALONE
int ppf, bin;
char binblock[1024], ppfblock[1024];
unsigned char ppfmem[512];
#else
extern int ppf, bin;
extern char binblock[1024], ppfblock[1024];
extern unsigned char ppfmem[512];
#endif
#define APPLY 1
#define UNDO 2

static void CleanupTempIfPresent(void) {
    if (!bin_is_temp) return;
    if (bin != -1) {
        _close(bin);
        bin = -1;
    }
    if (bin_tmp_name[0]) {
        DeleteFileA(bin_tmp_name);
    }
    bin_is_temp = 0;
}

static BOOL WINAPI ApplyConsoleCtrlHandler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
        case CTRL_C_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            CleanupTempIfPresent();
            return FALSE;
        default:
            return FALSE;
    }
}


int		OpenFiles(char* file1, char* file2);
int		PPFVersion(int ppf);
void	ApplyPPF1Patch(int ppf, int bin);
void	ApplyPPF2Patch(int ppf, int bin);
void	ApplyPPF3Patch(int ppf, int bin, char mode);
void ApplyPPF_Finalize(void);
static void (*ApplyPPF_ProgressCallback)(double) = NULL;
void ApplyPPF_SetProgressCallback(void (*cb)(double)) { ApplyPPF_ProgressCallback = cb; }
int		ShowFileId(int ppf);

typedef struct {
    unsigned char buf[64 * 1024];
    size_t len;
    __int64 offset;
    int active;
} write_agg_t;

static inline void agg_init(write_agg_t *a) {
    a->len = 0; a->offset = 0; a->active = 0;
}
static int agg_flush(write_agg_t *a, int fd) {
    if (!a->active) return 0;
    if (_lseeki64(fd, a->offset, SEEK_SET) == -1) return -1;
    if (safe_write(fd, a->buf, a->len) != 0) return -1;
    a->len = 0; a->active = 0; return 0;
}
static int agg_write(write_agg_t *a, int fd, __int64 offset, const void *data, size_t size) {
    static int env_checked = 0;
    static int disable_aggregate = 0;
    if (size == 0) return 0;
    if (!env_checked) {
        char *env_disable = getenv("PPFMANAGER_NO_AGGREGATE");
        if (env_disable && env_disable[0] == '1') disable_aggregate = 1;
        env_checked = 1;
    }
    if (disable_aggregate) {
        if (a->active) { if (agg_flush(a, fd) != 0) return -1; }
        if (_lseeki64(fd, offset, SEEK_SET) == -1) return -1;
        return safe_write(fd, data, size);
    }
    if (size > sizeof(a->buf)) {
        if (a->active) { if (agg_flush(a, fd) != 0) return -1; }
        if (_lseeki64(fd, offset, SEEK_SET) == -1) return -1;
        return safe_write(fd, data, size);
    }
    if (!a->active) {
        a->active = 1; a->offset = offset; memcpy(a->buf, data, size); a->len = size; return 0;
    }
    if (offset == (a->offset + (__int64)a->len) && (a->len + size) <= sizeof(a->buf)) {
        memcpy(a->buf + a->len, data, size); a->len += size; return 0;
    }
    if (agg_flush(a, fd) != 0) return -1;
    a->active = 1; a->offset = offset; memcpy(a->buf, data, size); a->len = size; return 0;
}
#ifdef BUILD_STANDALONE
#endif


#ifdef BUILD_STANDALONE
int main(int argc, char **argv)
#else
int ApplyPPF_Main(int argc, char **argv)
#endif
{
	SetConsoleCtrlHandler(ApplyConsoleCtrlHandler, TRUE);
	apply_safe_patch = 0;
	if (argc == 5 && argv[4] &&
		(strcmp(argv[4], "-s") == 0 || strcmp(argv[4], "--safe") == 0 || strcmp(argv[4], "-safe") == 0)) {
		apply_safe_patch = 1;
	}
	if(argc != 4 && argc != 5){
		printf("Usage: PPFManager.exe <command> <binfile> <patchfile> [-s]\n");
		printf("<Commands>\n");
		printf("  a : apply PPF1/2/3 patch\n");
		printf("  u : undo patch (PPF3 only)\n");

		printf("\nExample: PPFManager.exe a game.bin patch.ppf\n");
		printf("Options:\n");
		printf("  -s : safe patching (copy original to temp, replace atomically at the end)\n");
		return(0);
	}

	int x;
	switch(*argv[1]){
			case 'a'	:	if(OpenFiles(argv[2], argv[3])) return(0);
							x=PPFVersion(ppf);
							if(x){
								if(x==1){ ApplyPPF1Patch(ppf, bin); break; }
								if(x==2){ ApplyPPF2Patch(ppf, bin); break; }
								if(x==3){ ApplyPPF3Patch(ppf, bin, APPLY); break; }
							} else{ break; }
							break;
			case 'u'	:	if(OpenFiles(argv[2], argv[3])) return(0);
							x=PPFVersion(ppf);
							if(x){
								if(x!=3){
									printf("Undo function is supported by PPF3.0 only\n");
								} else {
									ApplyPPF3Patch(ppf, bin, UNDO);
								}
							} else{ break; }
							break;

			default		:
							printf("Error: unknown command: \"%s\"\n",argv[1]);
							return(0);
							break;
	}

	ApplyPPF_Finalize();
	return(0);
}

void ApplyPPF_Finalize(void) {
	if (bin_is_temp) {
		if (bin != -1) { _close(bin); bin = -1; }
		if (apply_success) {
			if (!MoveFileExA(bin_tmp_name, bin_orig_name, MOVEFILE_REPLACE_EXISTING)) {
				DWORD mverr = GetLastError();
				
                if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] MoveFileExA failed (GetLastError=%lu)\n", (unsigned long)mverr); fflush(stdout); }
				if (!CopyFileA(bin_tmp_name, bin_orig_name, FALSE)) {
					DWORD cperr = GetLastError();
					if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] CopyFileA failed (GetLastError=%lu)\n", (unsigned long)cperr); fflush(stdout); }
					PrintWin32ErrorFmt("Error: cannot replace original file '%s' with patched file '%s'", bin_orig_name, bin_tmp_name);
				} else {
					DeleteFileA(bin_tmp_name);
					if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] CopyFileA succeeded\n"); fflush(stdout); }
				}
			} else {
				if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] MoveFileExA succeeded\n"); fflush(stdout); }
			}
		} else {
			if (!DeleteFileA(bin_tmp_name)) {
				printf("Warning: failed to remove temporary file '%s'\n", bin_tmp_name);
			}
		}
		bin_is_temp = 0;
	} else {
		if (bin != -1) { _close(bin); bin = -1; }
	}
	if (ppf != -1) { _close(ppf); ppf = -1; }
}


void ApplyPPF1Patch(int ppf, int bin){
	apply_success = 0;
	char desc[51];
	int32_t pos32;
	__int64 pos = 0;
	__int64 count, seekpos;
	unsigned char anz;


	_lseeki64(ppf, 6,SEEK_SET);
	if (_read(ppf, desc, 50) != 50) { printf("Error: failed reading patch description\n"); return; }
	desc[50]=0;
	printf("Patchfile is a PPF1.0 patch. Patch Information:\n");
	PrintDescriptionBytes((unsigned char*)desc);
	printf("%-12s : no\n", PPFManager_FileIdNameA());

	printf("Progress: "); fflush(stdout);
	_lseeki64(ppf, 0, SEEK_END);
	count = _telli64(ppf);
	count -= 56;
	seekpos = 56;

	__int64 total_count = count;
	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(0.0);

	write_agg_t agg; agg_init(&agg);

	do{
		if (InterlockedCompareExchange(&g_app_closing,0,0) || InterlockedCompareExchange(&g_cancel_requested,0,0)) { printf("Aborting apply (The GUI closed or stopped)\n"); return; }
		if (count < 5) { printf("Error: patch data truncated\n"); return; }
		_lseeki64(ppf, seekpos, SEEK_SET);
		if (_read(ppf, &pos32, 4) != 4) { printf("Error: failed reading patch data\n"); return; }
		pos = (__int64)pos32;
		if (_read(ppf, &anz, 1) != 1) { printf("Error: failed reading patch data\n"); return; }
		if (_read(ppf, &ppfmem, anz) != anz) { printf("Error: failed reading patch data\n"); return; }
		if (agg_write(&agg, bin, pos, &ppfmem, anz) != 0) { agg_flush(&agg, bin); printf("Error: failed to write to target file\n"); return; }
		seekpos = seekpos + 5 + anz;
		count = count - 5 - anz;
		if (ApplyPPF_ProgressCallback && total_count > 0) {
			double pct = ((double)(total_count - count) / (double)total_count) * 100.0;
			ApplyPPF_ProgressCallback(pct);
		}
	} while(count != 0);

	if (agg_flush(&agg, bin) != 0) { printf("Error: failed to write aggregated data\n"); return; }
	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(100.0);
	printf(" 100.00 %%\n");
	printf("Done.\n");
	apply_success = 1;

}


void ApplyPPF2Patch(int ppf, int bin){
		char desc[51], in;
		__int64 binlen, count, seekpos;
	uint32_t obinlen;
	int idlen;
	unsigned char anz;
    _lseeki64(ppf, 6, SEEK_SET);
    if (_read(ppf, desc, 50) != 50) { printf("Error: failed reading patch description\n"); return; }
    desc[50]=0;
    PrintDescriptionBytes((unsigned char*)desc);
		printf("%-12s : ", PPFManager_FileIdNameA());
		idlen=ShowFileId(ppf);
		if(!idlen) printf("Not available\n");

		_lseeki64(ppf, 56, SEEK_SET);
		if (_read(ppf, &obinlen, 4) != 4) { printf("Error: failed reading patch metadata\n"); return; }

        _lseeki64(bin, 0, SEEK_END);
        binlen = _telli64(bin);
        if((__int64)obinlen != binlen){
			if (!PromptYesNo("The size of the bin file isn't correct, continue ? (y/n): ", 0)) { printf("Aborted...\n"); return; }
		}

		_lseeki64(ppf, 60, SEEK_SET);
		if (_read(ppf, &ppfblock, 1024) != 1024) { printf("Error: failed reading ppf validation block\n"); return; }
		{
			long long _seekret = _lseeki64(bin, 0x9320, SEEK_SET);
			long long _flen = _filelengthi64(bin);
			if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] bin seek ret=%lld filelen=%lld\n", _seekret, _flen); fflush(stdout); }
			int _rv = _read(bin, &binblock, 1024);
			if (_rv < 0) { printf("Warning: failed reading bin validation block (rv=%d errno=%d) - treating as zeros\n", _rv, errno); fflush(stdout); memset(binblock, 0, 1024); }
			else if (_rv < 1024) memset(binblock + _rv, 0, 1024 - _rv);
		}
		in=memcmp(ppfblock, binblock, 1024);
		if(in!=0){
			if (!PromptYesNo("Binblock/Patchvalidation failed. continue ? (y/n): ", 0)) { printf("Aborted...\n"); return; }
		}

		printf("Progress: "); fflush(stdout);
		_lseeki64(ppf, 0, SEEK_END);
	count = _telli64(ppf);
	seekpos=1084;
	count-=1084;
	if(idlen) count-=idlen+38;

	__int64 total_count = count > 0 ? count : 1;
	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(0.0);

	write_agg_t agg; agg_init(&agg);

        do{
		if (InterlockedCompareExchange(&g_app_closing,0,0) || InterlockedCompareExchange(&g_cancel_requested,0,0)) { printf("Aborting apply (The GUI closed or stopped)\n"); return; }
		if (count < 5) { printf("Error: patch data truncated\n"); return; }
		_lseeki64(ppf, seekpos, SEEK_SET);
		int32_t pos32;
		if (_read(ppf, &pos32, 4) != 4) { printf("Error: failed reading patch data\n"); return; }
		__int64 pos = (__int64)pos32;
		if (_read(ppf, &anz, 1) != 1) { printf("Error: failed reading patch data\n"); return; }
		if (_read(ppf, &ppfmem, anz) != anz) { printf("Error: failed reading patch data\n"); return; }
		{
			__int64 blen = _filelengthi64(bin);
			if ((pos < 0) || (pos + (__int64)anz > blen)) { printf("Error: invalid write range (pos=%lld, size=%u)\n", (long long)pos, anz); return; }
			if (agg_write(&agg, bin, pos, &ppfmem, anz) != 0) { agg_flush(&agg, bin); printf("Error: failed to write to target file\n"); return; }
		}
		seekpos=seekpos+5+anz;
		count=count-5-anz;
		if (ApplyPPF_ProgressCallback && total_count > 0) {
			double pct = ((double)(total_count - count) / (double)total_count) * 100.0;
			ApplyPPF_ProgressCallback(pct);
		}
        } while(count!=0);

	if (agg_flush(&agg, bin) != 0) { printf("Error: failed to write aggregated data\n"); return; }

	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(100.0);
	printf(" 100.00 %%\n");
	printf("Done.\n");
	apply_success = 1;
}

void ApplyPPF3Patch(int ppf, int bin, char mode){
	apply_success = 0;
	unsigned char desc[51], imagetype=0, undo=0, blockcheck=0, in;
	int idlen;
	__int64 offset, count;
	__int64 seekpos;
	unsigned char anz=0;
    int test_mode = 0;
    char *env_test = getenv("PPFMANAGER_TEST");
    if (env_test && env_test[0] == '1') test_mode = 1;

    unsigned long long __perf_t0 = 0;
    if (perf_enabled()) __perf_t0 = perf_now();
    _lseeki64(ppf, 6, SEEK_SET);
    if (_read(ppf, desc, 50) != 50) { printf("Error: failed reading patch description\n"); return; }
    desc[50]=0;
    printf("Patchfile is a PPF3.0 patch. Patch Information:\n");
    PrintDescriptionBytes((unsigned char*)desc);
	printf("%-14s : ", PPFManager_FileIdNameA());

	idlen=ShowFileId(ppf);
	if(!idlen) printf("Not available\n");

	_lseeki64(ppf, 56, SEEK_SET);
	if (_read(ppf, &imagetype, 1) != 1) { printf("Error: failed reading patch header\n"); return; }
	_lseeki64(ppf, 57, SEEK_SET);
	if (_read(ppf, &blockcheck, 1) != 1) { printf("Error: failed reading patch header\n"); return; }
	_lseeki64(ppf, 58, SEEK_SET);
	if (_read(ppf, &undo, 1) != 1) { printf("Error: failed reading patch header\n"); return; }

	if(mode==UNDO){
		if(!undo){
			printf("Error: no undo data available\n");
			return;
		}
	}

	if(blockcheck){
		_lseeki64(ppf, 60, SEEK_SET);
		if (_read(ppf, &ppfblock, 1024) != 1024) { printf("Error: failed reading ppf validation block\n"); return; }

		
		if(imagetype == 1){
			_lseeki64(bin, 0x80A0, SEEK_SET); 
		} else if(imagetype == 2){
			_lseeki64(bin, 0x8000, SEEK_SET); 
		} else {
			_lseeki64(bin, 0x9320, SEEK_SET); 
		}
		char *env_allow = getenv("PPFMANAGER_ALLOW_VALIDATION_FAIL");
		int allow_validation_fail = 0;
		if (env_allow && (_stricmp(env_allow, "1") == 0 || _stricmp(env_allow, "true") == 0)) allow_validation_fail = 1;
		long long _seekret = _lseeki64(bin, (__int64)0x9320, SEEK_SET);
		if (_seekret == -1) {
			printf("Warning: seek to validation offset failed - treating validation data as zeros\n"); fflush(stdout);
			memset(binblock, 0, 1024);
		} else {
			int _rv = _read(bin, &binblock, 1024);
			if (_rv <= 0) {
				printf("Warning: short read of bin validation block (%d) - treating missing bytes as zeros\n", _rv); fflush(stdout);
				memset(binblock, 0, 1024);
			} else if (_rv < 1024) {
				printf("Warning: short read of bin validation block (%d) - filling remaining with zeros\n", _rv); fflush(stdout);
				memset(binblock + _rv, 0, 1024 - _rv);
			}
		}
		in=memcmp(ppfblock, binblock, 1024);
		if(in!=0){
			if (allow_validation_fail) {
				printf("Warning: Binblock/Patch validation failed but PPFMANAGER_ALLOW_VALIDATION_FAIL is set, continuing.\n");
			} else if (!PromptYesNo("Binblock/Patchvalidation failed. continue ? (y/n): ", 0)) { printf("Aborted...\n"); return; }
		}
	}

	_lseeki64(ppf, 0, SEEK_END);
	count=_telli64(ppf);
	_lseeki64(ppf, 0, SEEK_SET);
	
	if(blockcheck){
		seekpos=1084;
		count-=1084;
	} else {
		seekpos=60;
		count-=60;
	}

	if(idlen) count-=(idlen+18+16+2);
	
	__int64 total_count = count > 0 ? count : 1;
	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(0.0);
	
	write_agg_t agg; agg_init(&agg);

	printf("Progress: "); fflush(stdout);
	_lseeki64(ppf, seekpos, SEEK_SET);
	do{
		if (InterlockedCompareExchange(&g_app_closing,0,0) || InterlockedCompareExchange(&g_cancel_requested,0,0)) { printf("Aborting apply (The GUI closed or stopped)\n"); return; }
		if (count < 9) { printf("Error: patch entry truncated or count mismatch\n"); return; }
		{
            unsigned long long offv = 0;
            if (read_le64(ppf, &offv) != 0) { printf("Error: failed reading patch data\n"); return; }
            offset = (__int64)offv;
        }
		if (_read(ppf, &anz, 1) != 1) { printf("Error: failed reading patch data\n"); return; }

		if(mode==APPLY){
			if (_read(ppf, &ppfmem, anz) != anz) { printf("Error: failed reading patch data\n"); return; }
			if(undo) _lseeki64(ppf, anz, SEEK_CUR);
		}

		if(mode==UNDO){
			_lseeki64(ppf, anz, SEEK_CUR);
			if (_read(ppf, &ppfmem, anz) != anz) { printf("Error: failed reading patch data\n"); return; }
		}

		{
			__int64 blen = _filelengthi64(bin);
			if (offset < 0 || offset + anz > blen) { printf("Error: invalid write range (offset=%lld, size=%u)\n", (long long)offset, anz); return; }
				if (test_mode) { printf("[DBG] about to write %u bytes at offset=%lld to tmpfile '%s' (first=%u)\n", anz, (long long)offset, bin_tmp_name, (unsigned)ppfmem[0]); fflush(stdout); }
			if (agg_write(&agg, bin, offset, &ppfmem, anz) != 0) { agg_flush(&agg, bin); printf("Error: failed to write to target file\n"); return; }
		}
		count-=(anz+9);
		if(undo) count-=anz;

		if (count < 0) { printf("Error: count underflow after applying entries\n"); return; }

		if (ApplyPPF_ProgressCallback && total_count > 0) {
			double pct = ((double)(total_count - count) / (double)total_count) * 100.0;
			ApplyPPF_ProgressCallback(pct);
		}
	} while(count!=0);

	if (agg_flush(&agg, bin) != 0) { printf("Error: failed to write aggregated data\n"); return; }

	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(100.0);
		printf(" 100.00 %%\n");

		if (perf_enabled()) perf_report_ms("ApplyPPF3Patch", __perf_t0, perf_now());
		printf("Done.\n");
		if (test_mode) { printf("[DBG] apply_success=1 tmpfile=%s\n", bin_tmp_name); fflush(stdout); }
		apply_success = 1;

}


int ShowFileId(int ppf){
	char buffer2[3073];
	char end_marker[] = "@END_FILE_ID.DIZ";
	unsigned char buf[18];
	unsigned short idlen = 0, orglen = 0;
	
	if(_lseeki64(ppf, -18, SEEK_END) == -1) {
		return 0;
	}
	if(_read(ppf, buf, 18) != 18) {
		return 0;
	}
	if (memcmp(buf, end_marker, 16) != 0) return 0;
	idlen = (unsigned short)(buf[16] | (buf[17] << 8));
	orglen = idlen;
	if (idlen == 0 || idlen > 3072) return 0;
	if(_lseeki64(ppf, -(idlen+18), SEEK_END) == -1) {
		return 0;
	}
	if(_read(ppf, buffer2, idlen) != idlen) {
		return 0;
	}
	buffer2[idlen]=0;
	printf("Available\n");
	PrintRawTextBytes((unsigned char*)buffer2);
	return orglen;
}


int PPFVersion(int ppf){
	unsigned char magic[4];

	_lseeki64(ppf,0,SEEK_SET);
	_read(ppf, magic, 4);
	if(memcmp(magic, "PPF1", 4) == 0) return(1);
	if(memcmp(magic, "PPF2", 4) == 0) return(2);
	if(memcmp(magic, "PPF3", 4) == 0) return(3);
	printf("Error: patchfile is no ppf patch\n");
	return(0);
}


int OpenFiles(char* file1, char* file2){

	bin = _open(file1, _O_BINARY | (apply_safe_patch ? _O_RDONLY : _O_RDWR));
	if(bin == -1){
		PrintWin32ErrorFmt("Error: cannot open file '%s'", file1);
		return(1);
	}
	strncpy(bin_orig_name, file1, sizeof(bin_orig_name)-1);
	bin_orig_name[sizeof(bin_orig_name)-1] = '\0';

	ppf = _open(file2, _O_RDONLY | _O_BINARY);
	if(ppf == -1){
		PrintWin32ErrorFmt("Error: cannot open file '%s'", file2);
		_close(bin);
		return(1);
	}

	if (ApplyPPF_ProgressCallback) {
		ApplyPPF_ProgressCallback(0.0);
	}

	if (apply_safe_patch) {
		int tmpfd = -1;
		char tmpdir[MAX_PATH];
		strncpy(tmpdir, file1, sizeof(tmpdir)-1);
		tmpdir[sizeof(tmpdir)-1] = '\0';
		char *p = strrchr(tmpdir, '\\');
		if (!p) p = strrchr(tmpdir, '/');
		if (p) *p = '\0';
		else if (!GetTempPathA(MAX_PATH, tmpdir)) {
			strcpy_s(tmpdir, sizeof(tmpdir), ".");
		}

		if (GetTempFileNameA(tmpdir, "ppf", 0, bin_tmp_name)) {
			tmpfd = _open(bin_tmp_name, _O_BINARY | _O_RDWR);
			if (tmpfd == -1) {
				remove(bin_tmp_name);
			}
		}

		if (tmpfd == -1) {
			int tryi;
			for(tryi = 0; tryi < 1000; tryi++){
				if(tryi == 0)
					_snprintf_s(bin_tmp_name, sizeof(bin_tmp_name), _TRUNCATE, "%s.ppf_tmp", file1);
				else
					_snprintf_s(bin_tmp_name, sizeof(bin_tmp_name), _TRUNCATE, "%s.ppf_tmp%03d", file1, tryi);
				tmpfd = _open(bin_tmp_name, _O_BINARY | _O_CREAT | _O_EXCL | _O_RDWR, _S_IREAD | _S_IWRITE);
				if(tmpfd != -1) break;
				if(errno != EEXIST) break;
			}
		}

		if(tmpfd == -1){
			printf("Error: cannot create temporary file for patching: %s\n", bin_tmp_name);
			_close(ppf);
			_close(bin);
			return(1);
		}

		_lseeki64(bin, 0, SEEK_SET);
		_lseeki64(tmpfd, 0, SEEK_SET);
		{
			char buf[65536];
			int r;
			__int64 copied = 0;
			__int64 total = _filelengthi64(bin);

			while((r = _read(bin, buf, sizeof(buf))) > 0){
				if (InterlockedCompareExchange(&g_cancel_requested, 0, 0)) {
					printf("Aborting copy (user cancelled)\n");
					_close(tmpfd);
					remove(bin_tmp_name);
					_close(ppf);
					_close(bin);
					return(1);
				}

				if (safe_write(tmpfd, buf, r) != 0) {
					printf("Error: failed writing to temporary patch file\n");
					_close(tmpfd);
					remove(bin_tmp_name);
					_close(ppf);
					_close(bin);
					return(1);
				}
				copied += r;
				if (ApplyPPF_ProgressCallback && total > 0) {
					double pct = ((double)copied / (double)total) * 90.0; 
					ApplyPPF_ProgressCallback(pct);
				}
			}
			if (r < 0) {
				printf("Error: failed reading source file during temp copy\n");
				_close(tmpfd);
				remove(bin_tmp_name);
				_close(ppf);
				_close(bin);
				return(1);
			}
		}
		_close(bin);
		bin = tmpfd;
		bin_is_temp = 1;
	}

	return(0);
}
