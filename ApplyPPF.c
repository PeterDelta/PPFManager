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


//////////////////////////////////////////////////////////////////////
// Used global variables.
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

//////////////////////////////////////////////////////////////////////
// Used prototypes.
int		OpenFiles(char* file1, char* file2);
int		PPFVersion(int ppf);
void	ApplyPPF1Patch(int ppf, int bin);
void	ApplyPPF2Patch(int ppf, int bin);
void	ApplyPPF3Patch(int ppf, int bin, char mode);
int		ShowFileId(int ppf, int ppfver);

/* Print description bytes: prefer UTF-8, fallback to ANSI (CP_ACP). Use WriteConsoleW when stdout is a console. */
static void PrintDescriptionBytes(const unsigned char *desc) {
    if (!desc) { printf("Description : \n"); return; }
    wchar_t *w = NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (char*)desc, -1, NULL, 0);
    if (wlen > 0) {
        w = (wchar_t*)malloc(wlen * sizeof(wchar_t));
        if (!w) { printf("Description : %s\n", desc); return; }
        MultiByteToWideChar(CP_UTF8, 0, (char*)desc, -1, w, wlen);
        int bad = 0; for (int i = 0; i < wlen && w[i]; ++i) if (w[i] == 0xFFFD) { bad = 1; break; }
        if (bad) { free(w); w = NULL; }
    }
    if (!w) {
        wlen = MultiByteToWideChar(CP_ACP, 0, (char*)desc, -1, NULL, 0);
        if (wlen > 0) {
            w = (wchar_t*)malloc(wlen * sizeof(wchar_t));
            if (!w) { printf("Description : %s\n", desc); return; }
            MultiByteToWideChar(CP_ACP, 0, (char*)desc, -1, w, wlen);
        }
    }

    if (w) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0, mode;
        if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
            WriteConsoleW(hOut, L"Description : ", (DWORD)wcslen(L"Description : "), &written, NULL);
            WriteConsoleW(hOut, w, (DWORD)wcslen(w), &written, NULL);
            WriteConsoleW(hOut, L"\n", 1, &written, NULL);
            free(w);
            return;
        }
        int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        char *out = (char*)malloc(need);
        if (!out) { free(w); printf("Description : %s\n", desc); return; }
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, need, NULL, NULL);
        printf("Description : %s\n", out);
        free(out);
        free(w);
        return;
    }
    printf("Description : %s\n", desc);
}

/* Print raw text bytes (no label): prefer UTF-8, fallback to ANSI, use WriteConsoleW when stdout is attached */
static void PrintRawTextBytes(const unsigned char *s) {
    if (!s) { printf("\n"); return; }
    wchar_t *w = NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (char*)s, -1, NULL, 0);
    if (wlen > 0) {
        w = (wchar_t*)malloc(wlen * sizeof(wchar_t));
        if (!w) { printf("%s\n", s); return; }
        MultiByteToWideChar(CP_UTF8, 0, (char*)s, -1, w, wlen);
        int bad = 0; for (int i = 0; i < wlen && w[i]; ++i) if (w[i] == 0xFFFD) { bad = 1; break; }
        if (bad) { free(w); w = NULL; }
    }
    if (!w) {
        wlen = MultiByteToWideChar(CP_ACP, 0, (char*)s, -1, NULL, 0);
        if (wlen > 0) {
            w = (wchar_t*)malloc(wlen * sizeof(wchar_t));
            if (!w) { printf("%s\n", s); return; }
            MultiByteToWideChar(CP_ACP, 0, (char*)s, -1, w, wlen);
        }
    }
    if (w) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0, mode;
        if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
            WriteConsoleW(hOut, w, (DWORD)wcslen(w), &written, NULL);
            WriteConsoleW(hOut, L"\n", 1, &written, NULL);
            free(w);
            return;
        }
        int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        char *out = (char*)malloc(need);
        if (!out) { free(w); printf("%s\n", s); return; }
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, need, NULL, NULL);
        printf("%s\n", out);
        free(out);
        free(w);
        return;
    }
    printf("%s\n", s);
}



#ifdef BUILD_STANDALONE
int main(int argc, char **argv)
#else
int ApplyPPF_Main(int argc, char **argv)
#endif
{
	printf("ApplyPPF v3.0 by =Icarus/Paradox= %s\n", __DATE__);
	if(argc!=4){
		printf("Usage: ApplyPPF <command> <binfile> <patchfile>\n");
		printf("<Commands>\n");
		printf("  a : apply PPF1/2/3 patch\n");
		printf("  u : undo patch (PPF3 only)\n");

		printf("\nExample: ApplyPPF.exe a game.bin patch.ppf\n");
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

	_close(bin);
	_close(ppf);
	return(0);
}

//////////////////////////////////////////////////////////////////////
// Applies a PPF1.0 patch.
void ApplyPPF1Patch(int ppf, int bin){
	char desc[51];
	int pos;
	unsigned int count, seekpos;
	unsigned char anz;


	_lseeki64(ppf, 6,SEEK_SET);  /* Read Desc.line */
	_read(ppf, desc, 50); desc[50]=0;
	printf("Patchfile is a PPF1.0 patch. Patch Information:\n");
	PrintDescriptionBytes((unsigned char*)desc);
	printf("File_id.diz : no\n");

	printf("Patching... "); fflush(stdout);
	_lseeki64(ppf, 0, SEEK_END);
	count=(unsigned int)_telli64(ppf);
	count-=56;
	seekpos=56;

	do{
		_lseeki64(ppf, seekpos, SEEK_SET);
		_read(ppf, &pos, 4);
		_read(ppf, &anz, 1);
		_read(ppf, &ppfmem, anz);
		_lseeki64(bin, pos, SEEK_SET);
		_write(bin, &ppfmem, anz);
		seekpos=seekpos+5+anz;
		count=count-5-anz;
	} while(count!=0);

	printf(" 100.00 %%\n");
	printf("successful.\n");

}

//////////////////////////////////////////////////////////////////////
// Applies a PPF2.0 patch.
void ApplyPPF2Patch(int ppf, int bin){
		char desc[51], in;
		unsigned int binlen, obinlen, count, seekpos;
		int idlen, pos;
		unsigned char anz;



		_lseeki64(ppf, 6,SEEK_SET);
		_read(ppf, desc, 50); desc[50]=0;
		printf("Patchfile is a PPF2.0 patch. Patch Information:\n");
	PrintDescriptionBytes((unsigned char*)desc);
		printf("File_id.diz : ");
		idlen=ShowFileId(ppf, 2);
		if(!idlen) printf("Not available\n");

		_lseeki64(ppf, 56, SEEK_SET);
		_read(ppf, &obinlen, 4);

        _lseeki64(bin, 0, SEEK_END);
        binlen=(unsigned int)_telli64(bin);
        if(obinlen!=binlen){
			printf("The size of the bin file isn't correct, continue ? (y/n): "); fflush(stdout);
			in=getc(stdin);
			if(in!='y'&&in!='Y'){
				printf("Aborted...\n");
				return;
			}
		}

		_lseeki64(ppf, 60, SEEK_SET);
		_read(ppf, &ppfblock, 1024);
		_lseeki64(bin, 0x9320, SEEK_SET);
		_read(bin, &binblock, 1024);
		in=memcmp(ppfblock, binblock, 1024);
		if(in!=0){
			printf("Binblock/Patchvalidation failed. continue ? (y/n): "); fflush(stdout);
			in=getc(stdin);
			if(in!='y'&&in!='Y'){
				printf("Aborted...\n");
				return;
			}
		}

		printf("Patching... "); fflush(stdout);
		_lseeki64(ppf, 0, SEEK_END);
	count=(unsigned int)_telli64(ppf);
	seekpos=1084;
	count-=1084;
	if(idlen) count-=idlen+38;

        do{
		_lseeki64(ppf, seekpos, SEEK_SET);
		_read(ppf, &pos, 4);
		_read(ppf, &anz, 1);
		_read(ppf, &ppfmem, anz);
		_lseeki64(bin, pos, SEEK_SET);
		_write(bin, &ppfmem, anz);
		seekpos=seekpos+5+anz;
		count=count-5-anz;
        } while(count!=0);

	printf(" 100.00 %%\n");
	printf("successful.\n");
}
//////////////////////////////////////////////////////////////////////
// Applies a PPF3.0 patch.
void ApplyPPF3Patch(int ppf, int bin, char mode){
	unsigned char desc[51], imagetype=0, undo=0, blockcheck=0, in;
	int idlen;
	__int64 offset, count;
	unsigned int seekpos;
	unsigned char anz=0;


	_lseeki64(ppf, 6,SEEK_SET);  /* Read Desc.line */
	_read(ppf, desc, 50); desc[50]=0;
	printf("Patchfile is a PPF3.0 patch. Patch Information:\n");
	PrintDescriptionBytes((unsigned char*)desc);
	printf("File_id.diz : ");

	idlen=ShowFileId(ppf, 3);
	if(!idlen) printf("Not available\n");

	_lseeki64(ppf, 56, SEEK_SET);
	_read(ppf, &imagetype, 1);
	_lseeki64(ppf, 57, SEEK_SET);
	_read(ppf, &blockcheck, 1);
	_lseeki64(ppf, 58, SEEK_SET);
	_read(ppf, &undo, 1);

	if(mode==UNDO){
		if(!undo){
			printf("Error: no undo data available\n");
			return;
		}
	}

	if(blockcheck){
		_lseeki64(ppf, 60, SEEK_SET);
		_read(ppf, &ppfblock, 1024);

		if(imagetype){
			_lseeki64(bin, 0x80A0, SEEK_SET);
		} else {
			_lseeki64(bin, 0x9320, SEEK_SET);
		}
		_read(bin, &binblock, 1024);
		in=memcmp(ppfblock, binblock, 1024);
		if(in!=0){
			printf("Binblock/Patchvalidation failed. continue ? (y/n): "); fflush(stdout);
			in=getc(stdin);
			if(in!='y'&&in!='Y'){
				printf("Aborted...\n");
				return;
			}
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
	

	printf("Patching ... "); fflush(stdout);
	_lseeki64(ppf, seekpos, SEEK_SET);
	do{
		_read(ppf, &offset, 8);
		_read(ppf, &anz, 1);

		if(mode==APPLY){
			_read(ppf, &ppfmem, anz);
			if(undo) _lseeki64(ppf, anz, SEEK_CUR);
		}

		if(mode==UNDO){
			_lseeki64(ppf, anz, SEEK_CUR);
			_read(ppf, &ppfmem, anz);
		}

		_lseeki64(bin, offset, SEEK_SET);
		_write(bin, &ppfmem, anz);
		count-=(anz+9);
		if(undo) count-=anz;

	} while(count!=0);

		printf(" 100.00 %%\n");

		printf("successful.\n");

}


//////////////////////////////////////////////////////////////////////
// Shows File_Id.diz of a PPF2.0 / PPF3.0 patch.
// Input: 2 = PPF2.0
// Input: 3 = PPF3.0
// Return 0 = Error/no fileid.
// Return>0 = Length of fileid.
int ShowFileId(int ppf, int ppfver){
	char buffer2[3073];
	unsigned char end_marker[16] = "@END_FILE_ID.DIZ";
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
	idlen = buf[16] | (buf[17] << 8);
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

//////////////////////////////////////////////////////////////////////
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


//////////////////////////////////////////////////////////////////////
// Open all needed files.
// Return: 0 - Successful
// Return: 1 - Failed.
int OpenFiles(char* file1, char* file2){

	bin=_open(file1, _O_BINARY | _O_RDWR);
	if(bin==-1){
		printf("Error: cannot open file '%s': ",file1); perror("");
		return(1);
	}

	ppf=_open(file2,  _O_RDONLY | _O_BINARY);
	if(ppf==-1){
		printf("Error: cannot open file '%s': ",file2); perror("");
		_close(bin);
		return(1);
	}

	return(0);
}
