/*
 * File Transfer Get file support for Altair 8800
 * BDS C 1.6 on CP/M
 *
 * Rewritten for BDS C constraints:
 *  - All symbols unique within first 7 characters
 *  - K&R style definitions only
 *  - No support for casts
 *  - No support for goto labels named 'end'
 */

#include <stdio.h>

#define FT_VERSION "1.00"

/* Port definitions */
#define FT_CPRT 60
#define FT_DPRT 61

/* Commands for port 60 */
#define FT_NOP 0
#define FT_SETFN 1
#define FT_GETCH 3
#define FT_CLOSE 4

/* Status values from port 60 */
#define FT_IDLE 0
#define FT_DATA 1
#define FT_EOF 2
#define FT_BUSY 3
#define FT_ERR 255

/* Function prototypes */
int inp();
int outp();
int fputc();

int ftget(filename, fp_out, bwrit, binary)
char* filename;
FILE* fp_out;
int* bwrit;
int binary;
{
    int i, len, status, byte, count, chunk_len, pad, wlen;

    count = 0;
    wlen = 0;

    /* Start filename transfer */
    outp(FT_CPRT, FT_SETFN);

    /* Send filename characters via data port */
    len = strlen(filename);
    for (i = 0; i < len; i++)
    {
        outp(FT_DPRT, filename[i]);
    }
    /* Send null terminator */
    outp(FT_DPRT, 0);

    /* Wait for response */
    do
    {
        status = inp(FT_CPRT) & 255;
    } while (status == FT_BUSY);
    if (status == FT_ERR)
    {
        return -1;
    }

    /* Request and read chunks */
    while (1)
    {
        /* Request next chunk */
        outp(FT_CPRT, FT_GETCH);

        /* Wait for data or EOF */
        while (1)
        {
            status = inp(FT_CPRT) & 255;
            if (status == FT_DATA || status == FT_EOF || status == FT_ERR)
            {
                break;
            }
        }

        if (status == FT_ERR)
        {
            outp(FT_CPRT, FT_CLOSE);
            return -1;
        }

        /* If EOF with no data, we're done */
        if (status == FT_EOF)
        {
            break;
        }

        /* Read count byte (0 means 256) */
        chunk_len = inp(FT_DPRT) & 255;
        if (chunk_len == 0)
        {
            chunk_len = 256;
        }

        /* Read exactly count bytes from data port */
        for (i = 0; i < chunk_len; i++)
        {
            byte = inp(FT_DPRT) & 255;
            count++;
            if (binary)
            {
                fputc(byte, fp_out);
                wlen++;
            }
            else if (byte == 10)
            {
                /* LF -> CR LF for canonical CP/M line endings */
                fputc(13, fp_out);
                fputc(10, fp_out);
                wlen += 2;
            }
            else if (byte != 13)
            {
                /* ordinary text byte (incoming CR is stripped) */
                fputc(byte, fp_out);
                wlen++;
            }
        }

        /* Check if that was the last chunk */
        status = inp(FT_CPRT) & 255;
        if (status == FT_EOF)
        {
            break;
        }
        else if (status == FT_ERR)
        {
            outp(FT_CPRT, FT_CLOSE);
            return -1;
        }
    }

    /* Close transfer */
    outp(FT_CPRT, FT_CLOSE);

    /* Text mode writes a single ^Z end-of-text marker. Both modes pad the  */
    /* final partial CP/M record with NUL so the saved file is well-formed; */
    /* BDS C does not zero-fill the last record itself. The reported count  */
    /* stays the real number of received bytes.                            */
    if (!binary)
    {
        fputc(CPMEOF, fp_out);
        wlen++;
    }
    pad = wlen & 127;
    if (pad != 0)
    {
        pad = 128 - pad;
        while (pad != 0)
        {
            fputc(0, fp_out);
            pad--;
        }
    }

    if (bwrit != 0)
    {
        *bwrit = count;
    }

    return 0;
}

int main(argc, argv)
int argc;
char** argv;
{
    int result, bwrit, binary;
    FILE* fp_out;
    char* filename;
    char* save_fn;
    char* piter;

    result = 0;
    bwrit = 0;
    binary = 0;

    if (argc == 1)
    {
        printf("FT (File Transfer) - Remote File Transfer v%s\n", FT_VERSION);
        printf("Transfer files from Remote FT Server\n\n");
        printf("Usage: ft [-g <filename>] [-gb <filename>]\n");
        printf("\nOptions:\n");
        printf("  -g  <filename>  Get a text file (normalize line endings)\n");
        printf("  -gb <filename>  Get a binary file (verbatim bytes)\n");
        printf("\nExamples:\n");
        printf("  ft -g test.txt       Download text file from server\n");
        printf("  ft -gb prog.com      Download binary file from server\n");
        return 0;
    }

    if (argc == 3)
    {
        if (strcmp(argv[1], "-gb") == 0 || strcmp(argv[1], "-GB") == 0)
        {
            binary = 1;
        }
        if (binary || strcmp(argv[1], "-g") == 0 || strcmp(argv[1], "-G") == 0)
        {
            filename = argv[2];
            save_fn = filename;
            piter = filename;

            /* Extract basename for local save */
            while (*piter != '\0')
            {
                if (*piter == '/' || *piter == '\\')
                {
                    if (*(piter + 1) != '\0')
                    {
                        save_fn = piter + 1;
                    }
                }
                piter++;
            }

            printf("Downloading '%s' from Remote FT Server...\n", filename);
            if (save_fn != filename)
            {
                printf("Saving as '%s'\n", save_fn);
            }

            if ((fp_out = fopen(save_fn, "wb")) == NULL)
            {
                printf("Error: Failed to create output file '%s'\n", save_fn);
                return -1;
            }

            /* Clear the text flag so the buffered I/O layer never inserts */
            /* its own ^Z or CR; ftget does all line-ending handling.      */
            fp_out->_flags &= ~_TEXT;

            result = ftget(filename, fp_out, &bwrit, binary);
            fclose(fp_out);

            if (result == 0)
            {
                printf("Done (%d bytes)\n", bwrit);
            }
            else
            {
                printf("Failed - file not found or server error\n");
                unlink(save_fn);
                return -1;
            }
            return 0;
        }
    }

    printf("Invalid arguments. Use 'ft' for help.\n");
    return -1;
}
