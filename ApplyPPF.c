/*
 *     ApplyPPF.c
 *     written by Icarus/Paradox
 *     suggestions and some fixes by <Hu Kares>, thanks.
 *
 *     Applies PPF1.0, PPF2.0 & PPF3.0 Patches (including PPF3.0 Undo support)
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
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <io.h>
#include <ctype.h>
#include <limits.h>
#include "ppfmanager.h"

// Print Win32 error with formatted prefix. Uses FormatMessage to show readable system message.
#ifndef BUILD_STANDALONE
/* Use shared PrintWin32ErrorFmt from PPFManager */
#else
/* Standalone implementations provided via ppfmanager.h to keep behavior consistent */
#endif 

#ifdef BUILD_STANDALONE
/* safe_write is provided centrally via ppfmanager.h to avoid duplication */
#endif

#ifdef BUILD_STANDALONE
/* PromptYesNo provided centrally in ppfmanager.h */
#endif

// Used global variables.
static int bin_is_temp = 0;
static char bin_orig_name[512] = {0};
static char bin_tmp_name[512] = {0};
static int apply_success = 0; 
/* Expose status to callers: 1 = last apply succeeded, 0 = failed or not attempted */
int ApplyPPF_GetSuccess(void) { return apply_success; }
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

/* Perform cleanup of temporary files created by OpenFiles(). This is safe to call from
   the console control handler; keep logic simple and reentrant. */
static void CleanupTempIfPresent(void) {
    if (!bin_is_temp) return;
    /* Close temp file descriptor if open */
    if (bin != -1) {
        _close(bin);
        bin = -1;
    }
    /* Attempt to delete temp file */
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
            /* Best-effort cleanup */
            CleanupTempIfPresent();
            /* Let the system continue with default handling */
            return FALSE;
        default:
            return FALSE;
    }
}

// Used prototypes.
int		OpenFiles(char* file1, char* file2);
int		PPFVersion(int ppf);
void	ApplyPPF1Patch(int ppf, int bin);
void	ApplyPPF2Patch(int ppf, int bin);
void	ApplyPPF3Patch(int ppf, int bin, char mode);
/* Expose finalizer so in-process callers can replace temp->original and cleanup */
void	ApplyPPF_Finalize(void);

/* Progress callback for GUI integration: percent in range 0.0 .. 100.0 */
static void (*ApplyPPF_ProgressCallback)(double) = NULL;
void ApplyPPF_SetProgressCallback(void (*cb)(double)) { ApplyPPF_ProgressCallback = cb; }
int		ShowFileId(int ppf);

/* Cleanup/handler helpers to ensure temporary files are removed on Ctrl-C/close events.
   We install a console control handler in the main entry so interruptions do not leave temp files. */
#ifdef BUILD_STANDALONE
/* PrintDescriptionBytes/PrintRawTextBytes provided via centralized ppfmanager.h */
#endif


#ifdef BUILD_STANDALONE
int main(int argc, char **argv)
#else
int ApplyPPF_Main(int argc, char **argv)
#endif
{
	/* Install cleanup handler for console control events so interruptions do not leave temporary files. */
	SetConsoleCtrlHandler(ApplyConsoleCtrlHandler, TRUE);
	if(argc!=4){
		printf("Usage: PPFManager.exe <command> <binfile> <patchfile>\n");
		printf("<Commands>\n");
		printf("  a : apply PPF1/2/3 patch\n");
		printf("  u : undo patch (PPF3 only)\n");

		printf("\nExample: PPFManager.exe a game.bin patch.ppf\n");
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

	/* Finalize processing of opened files: perform temp->original replace or cleanup. */
	ApplyPPF_Finalize();
	return(0);
}

/* Finalizer helper: perform temp replacement or cleanup and close handles.
   Exposed so callers that use ApplyPPF in-process (e.g., PPFManager) can invoke the same cleanup
   logic that standalone ApplyPPF main() used to perform. */
void ApplyPPF_Finalize(void) {
	if (bin_is_temp) {
		/* Close temp file descriptor first if still open */
		if (bin != -1) { _close(bin); bin = -1; }
		if (apply_success) {
			/* Try atomic replace first */
			if (!MoveFileExA(bin_tmp_name, bin_orig_name, MOVEFILE_REPLACE_EXISTING)) {
				DWORD mverr = GetLastError();
				if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] MoveFileExA failed (GetLastError=%lu)\n", (unsigned long)mverr); fflush(stdout); }
				/* Fallback: try copy then delete */
				if (!CopyFileA(bin_tmp_name, bin_orig_name, FALSE)) {
					DWORD cperr = GetLastError();
					if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] CopyFileA failed (GetLastError=%lu)\n", (unsigned long)cperr); fflush(stdout); }
					PrintWin32ErrorFmt("Error: cannot replace original file '%s' with patched file '%s'", bin_orig_name, bin_tmp_name);
					/* leave temp for inspection */
				} else {
					DeleteFileA(bin_tmp_name);
					if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] CopyFileA succeeded\n"); fflush(stdout); }
				}
			} else {
				if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] MoveFileExA succeeded\n"); fflush(stdout); }
			}
		} else {
			/* Apply failed or was interrupted: remove temp file to avoid leaving stale data */
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

// Applies a PPF1.0 patch.
void ApplyPPF1Patch(int ppf, int bin){
	/* reset status before attempting apply */
	apply_success = 0;
	char desc[51];
	int32_t pos32;
	__int64 pos = 0;
	__int64 count, seekpos;
	unsigned char anz;


	_lseeki64(ppf, 6,SEEK_SET);  /* Read Desc.line */
	if (_read(ppf, desc, 50) != 50) { printf("Error: failed reading patch description\n"); return; }
	desc[50]=0;
	printf("Patchfile is a PPF1.0 patch. Patch Information:\n");
	PrintDescriptionBytes((unsigned char*)desc);
	printf("File_id.diz : no\n");

	printf("Patching... "); fflush(stdout);
	_lseeki64(ppf, 0, SEEK_END);
	count = _telli64(ppf);
	count -= 56;
	seekpos = 56;

	/* Setup progress reporting for GUI */
	__int64 total_count = count;
	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(0.0);

	do{
		if (count < 5) { printf("Error: patch data truncated\n"); return; }
		_lseeki64(ppf, seekpos, SEEK_SET);
		if (_read(ppf, &pos32, 4) != 4) { printf("Error: failed reading patch data\n"); return; }
		pos = (__int64)pos32;
		if (_read(ppf, &anz, 1) != 1) { printf("Error: failed reading patch data\n"); return; }
		if (_read(ppf, &ppfmem, anz) != anz) { printf("Error: failed reading patch data\n"); return; }
		_lseeki64(bin, pos, SEEK_SET);
		if (safe_write(bin, &ppfmem, anz) != 0) { printf("Error: failed to write to target file\n"); return; }
		seekpos = seekpos + 5 + anz;
		count = count - 5 - anz;
		/* Update progress */
		if (ApplyPPF_ProgressCallback && total_count > 0) {
			double pct = ((double)(total_count - count) / (double)total_count) * 100.0;
			ApplyPPF_ProgressCallback(pct);
		}
	} while(count != 0);

	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(100.0);
	printf(" 100.00 %%\n");
	printf("Done.\n");
	apply_success = 1;

}

// Applies a PPF2.0 patch.
void ApplyPPF2Patch(int ppf, int bin){
		char desc[51], in;
		__int64 binlen, count, seekpos;
	uint32_t obinlen;
	int idlen;
	unsigned char anz;

	if (_read(ppf, desc, 50) != 50) { printf("Error: failed reading patch description\n"); return; }
	desc[50]=0;
	PrintDescriptionBytes((unsigned char*)desc);
		printf("File_id.diz : ");
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
			printf("[DBG] bin seek ret=%lld filelen=%lld\n", _seekret, _flen); fflush(stdout);
			int _rv = _read(bin, &binblock, 1024);
			if (_rv < 0) { printf("Warning: failed reading bin validation block (rv=%d errno=%d) - treating as zeros\n", _rv, errno); fflush(stdout); memset(binblock, 0, 1024); }
			else if (_rv < 1024) memset(binblock + _rv, 0, 1024 - _rv);
		}
		in=memcmp(ppfblock, binblock, 1024);
		if(in!=0){
			if (!PromptYesNo("Binblock/Patchvalidation failed. continue ? (y/n): ", 0)) { printf("Aborted...\n"); return; }
		}

		printf("Patching... "); fflush(stdout);
		_lseeki64(ppf, 0, SEEK_END);
	count = _telli64(ppf);
	seekpos=1084;
	count-=1084;
	if(idlen) count-=idlen+38;

	/* Setup progress reporting for GUI */
	__int64 total_count = count > 0 ? count : 1;
	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(0.0);

        do{
		if (count < 5) { printf("Error: patch data truncated\n"); return; }
		_lseeki64(ppf, seekpos, SEEK_SET);
		int32_t pos32;
		if (_read(ppf, &pos32, 4) != 4) { printf("Error: failed reading patch data\n"); return; }
		__int64 pos = (__int64)pos32;
		if (_read(ppf, &anz, 1) != 1) { printf("Error: failed reading patch data\n"); return; }
		if (_read(ppf, &ppfmem, anz) != anz) { printf("Error: failed reading patch data\n"); return; }
		{
			/* Validate target write range */
			__int64 blen = _filelengthi64(bin);
			if ((pos < 0) || (pos + (__int64)anz > blen)) { printf("Error: invalid write range (pos=%lld, size=%u)\n", (long long)pos, anz); return; }
			_lseeki64(bin, pos, SEEK_SET);
			if (safe_write(bin, &ppfmem, anz) != 0) { printf("Error: failed to write to target file\n"); return; }
		}
		seekpos=seekpos+5+anz;
		count=count-5-anz;
		/* Update progress */
		if (ApplyPPF_ProgressCallback && total_count > 0) {
			double pct = ((double)(total_count - count) / (double)total_count) * 100.0;
			ApplyPPF_ProgressCallback(pct);
		}
        } while(count!=0);

	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(100.0);
	printf(" 100.00 %%\n");
	printf("Done.\n");
	apply_success = 1;
}
// Applies a PPF3.0 patch.
void ApplyPPF3Patch(int ppf, int bin, char mode){
	/* reset status before attempting apply */
	apply_success = 0;
	unsigned char desc[51], imagetype=0, undo=0, blockcheck=0, in;
	int idlen;
	__int64 offset, count;
	__int64 seekpos;
	unsigned char anz=0;


	_lseeki64(ppf, 6,SEEK_SET);  /* Read Desc.line */
	if (_read(ppf, desc, 50) != 50) { printf("Error: failed reading patch description\n"); return; }
	desc[50]=0;
	printf("Patchfile is a PPF3.0 patch. Patch Information:\n");
	PrintDescriptionBytes((unsigned char*)desc);
	printf("File_id.diz : ");

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

		/* imagetype: 0=BIN, 1=GI, 2=ISO */
		if(imagetype == 1){
			_lseeki64(bin, 0x80A0, SEEK_SET); /* GI */
		} else if(imagetype == 2){
			_lseeki64(bin, 0x8000, SEEK_SET); /* ISO9660 PVD */
		} else {
			_lseeki64(bin, 0x9320, SEEK_SET); /* BIN */
		}
		/* Allow skipping validation prompt via environment variable */
		char *env_allow = getenv("PPFMANAGER_ALLOW_VALIDATION_FAIL");
		int allow_validation_fail = 0;
		if (env_allow && (_stricmp(env_allow, "1") == 0 || _stricmp(env_allow, "true") == 0)) allow_validation_fail = 1;
		long long _seekret = _lseeki64(bin, (__int64)0x9320, SEEK_SET);
		if (_seekret == -1) {
			/* Seek failed: file too small or seeking not supported. Treat missing validation data as zeros and continue. */
			printf("Warning: seek to validation offset failed - treating validation data as zeros\n"); fflush(stdout);
			memset(binblock, 0, 1024);
		} else {
			int _rv = _read(bin, &binblock, 1024);
			if (_rv <= 0) {
				/* Short/empty read: file does not contain validation region. Fill with zeros but continue. */
				printf("Warning: short read of bin validation block (%d) - treating missing bytes as zeros\n", _rv); fflush(stdout);
				memset(binblock, 0, 1024);
			} else if (_rv < 1024) {
				/* Partial read: fill remaining bytes with zeros and continue. */
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
	
	/* Setup progress reporting for GUI */
	__int64 total_count = count > 0 ? count : 1;
	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(0.0);
	
	printf("Patching ... "); fflush(stdout);
	_lseeki64(ppf, seekpos, SEEK_SET);
	do{
		/* Each PPF3 entry requires at least 9 bytes (offset 8 + size 1). */
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
			/* Validate target write range */
			__int64 blen = _filelengthi64(bin);
			if (offset < 0 || offset + anz > blen) { printf("Error: invalid write range (offset=%lld, size=%u)\n", (long long)offset, anz); return; }
			_lseeki64(bin, offset, SEEK_SET);				/* Test/debug logging (enabled when PPFMANAGER_TEST=1) to help diagnose write issues */
				if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] about to write %u bytes at offset=%lld to tmpfile '%s' (first=%u)\n", anz, (long long)offset, bin_tmp_name, (unsigned)ppfmem[0]); fflush(stdout); }			if (safe_write(bin, &ppfmem, anz) != 0) { printf("Error: failed to write to target file\n"); return; }
		}
		count-=(anz+9);
		if(undo) count-=anz;

		if (count < 0) { printf("Error: count underflow after applying entries\n"); return; }

		/* Update progress */
		if (ApplyPPF_ProgressCallback && total_count > 0) {
			double pct = ((double)(total_count - count) / (double)total_count) * 100.0;
			ApplyPPF_ProgressCallback(pct);
		}

	} while(count!=0);

	if (ApplyPPF_ProgressCallback) ApplyPPF_ProgressCallback(100.0);
		printf(" 100.00 %%\n");

		printf("Done.\n");
		/* Debug: report we are marking success and the temp filename being used (test-only) */
		if (getenv("PPFMANAGER_TEST") && getenv("PPFMANAGER_TEST")[0] == '1') { printf("[DBG] apply_success=1 tmpfile=%s\n", bin_tmp_name); fflush(stdout); }
		apply_success = 1;

}

// Shows File_Id.diz of a PPF2.0 / PPF3.0 patch.
// Input: 2 = PPF2.0
// Input: 3 = PPF3.0
// Return 0 = Error/no fileid.
// Return>0 = Length of fileid.
int ShowFileId(int ppf){
	char buffer2[3073];
	char end_marker[] = "@END_FILE_ID.DIZ";
	unsigned char buf[18];
	unsigned short idlen = 0, orglen = 0;
	// Only support the modern ending marker format (PPF3 style)
	// Read the last 18 bytes (16 marker + 2 length)
	if(_lseeki64(ppf, -18, SEEK_END) == -1) {
		return 0;
	}
	if(_read(ppf, buf, 18) != 18) {
		return 0;
	}
	// Verify marker
	if (memcmp(buf, end_marker, 16) != 0) return 0;
	// length is little-endian 16-bit stored in buf[16..17]
	idlen = (unsigned short)(buf[16] | (buf[17] << 8));
	orglen = idlen;
	if (idlen == 0 || idlen > 3072) return 0;
	// Read the content
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

// Check what PPF version we have.
// Return: 0 - File is no PPF.
// Return: 1 - File is a PPF1.0
// Return: 2 - File is a PPF2.0
// Return: 3 - File is a PPF3.0
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

// Open all needed files.
// Return: 0 - Successful
// Return: 1 - Failed.
int OpenFiles(char* file1, char* file2){

	bin=_open(file1, _O_BINARY | _O_RDONLY);
	if(bin==-1){
		PrintWin32ErrorFmt("Error: cannot open file '%s'", file1);
		return(1);
	}
	strncpy(bin_orig_name, file1, sizeof(bin_orig_name)-1);
	bin_orig_name[sizeof(bin_orig_name)-1] = '\0';

	ppf=_open(file2,  _O_RDONLY | _O_BINARY);
	if(ppf==-1){
		PrintWin32ErrorFmt("Error: cannot open file '%s'", file2);
		_close(bin);
		return(1);
	}

	/* Create a temp copy of original file and operate on the temp file. This avoids corrupting the original on failure. */
	{
		int tmpfd = -1;
		char tmpdir[MAX_PATH];
		/* Try to create temp file in same directory as target */
		strncpy(tmpdir, file1, sizeof(tmpdir)-1); tmpdir[sizeof(tmpdir)-1] = '\0';
		char *p = strrchr(tmpdir, '\\'); if (!p) p = strrchr(tmpdir, '/');
		if (p) *p = '\0'; else if (!GetTempPathA(MAX_PATH, tmpdir)) { strcpy_s(tmpdir, sizeof(tmpdir), "."); }

		if (GetTempFileNameA(tmpdir, "ppf", 0, bin_tmp_name)) {
			/* GetTempFileName created the file; open it for r/w */
			tmpfd = _open(bin_tmp_name, _O_BINARY | _O_RDWR);
			if (tmpfd == -1) { remove(bin_tmp_name); }
		}

		/* Fallback to original exclusive create loop if needed */
		if (tmpfd == -1) {
			int tryi;
			for(tryi=0; tryi<1000; tryi++){
			if(tryi==0) _snprintf_s(bin_tmp_name, sizeof(bin_tmp_name), _TRUNCATE, "%s.ppf_tmp", file1);
			else _snprintf_s(bin_tmp_name, sizeof(bin_tmp_name), _TRUNCATE, "%s.ppf_tmp%03d", file1, tryi);
				tmpfd = _open(bin_tmp_name, _O_BINARY | _O_CREAT | _O_EXCL | _O_RDWR, _S_IREAD | _S_IWRITE);
				if(tmpfd!=-1) break;
				if(errno!=EEXIST) break;
			}
		}

		if(tmpfd==-1){
			printf("Error: cannot create temporary file for patching: %s\n", bin_tmp_name);
			_close(ppf); _close(bin);
			return(1);
		}
		/* Copy original content to temp */
		_lseeki64(bin, 0, SEEK_SET);
		_lseeki64(tmpfd, 0, SEEK_SET);
		{
			char buf[65536];
			int r;
			while((r = _read(bin, buf, sizeof(buf))) > 0){
				if (safe_write(tmpfd, buf, r) != 0) {
					printf("Error: failed writing to temporary patch file\n");
					_close(tmpfd); remove(bin_tmp_name); _close(ppf); _close(bin);
					return(1);
				}
			}
			if (r < 0) { printf("Error: failed reading source file during temp copy\n"); _close(tmpfd); remove(bin_tmp_name); _close(ppf); _close(bin); return(1); }
		}
		/* Close original and switch bin to temp for in-place patching */
		_close(bin);
		bin = tmpfd;
		bin_is_temp = 1;
	}

	return(0);
}
