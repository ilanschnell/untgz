/*
 * tgz_extract functions based on code within zlib library
 * No additional copyright added, KJD <jeremyd@computer.org>
 *
 *   This software is provided 'as-is', without any express or implied
 *   warranty.  In no event will the authors be held liable for any damages
 *   arising from the use of this software.
 *
 * untgz.c -- Display contents and/or extract file from
 * a gzip'd TAR file
 * written by "Pedro A. Aranda Guti\irrez" <paag@tid.es>
 * adaptation to Unix by Jean-loup Gailly <jloup@gzip.org>
 * various fixes by Cosmin Truta <cosmint@cs.ubbcluj.ro>
*/

/*
  For tar format see
  http://cdrecord.berlios.de/old/private/man/star/star.4.html
  http://www.mkssoftware.com/docs/man4/tar.4.asp
  http://www.delorie.com/gnu/docs/tar/tar_toc.html

  TODO:
    without -j there is a security issue as no checking is done to directories
    change to better support -d option, presently we just chdir there
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>


#include "t.h"


/** the rest heavily based on (ie mostly) untgz.c from zlib **/

/* Values used in typeflag field.  */

#define REGTYPE  '0'            /* regular file */
#define AREGTYPE '\0'           /* regular file */
#define LNKTYPE  '1'            /* link */
#define SYMTYPE  '2'            /* reserved */
#define CHRTYPE  '3'            /* character special */
#define BLKTYPE  '4'            /* block special */
#define DIRTYPE  '5'            /* directory */
#define FIFOTYPE '6'            /* FIFO special */
#define CONTTYPE '7'            /* reserved, for compatibility with gnu tar,
                               treat as regular file, where it represents
                               a regular file, but saved contiguously on disk */

/* GNU tar extensions */

#define GNUTYPE_DUMPDIR  'D'    /* file names from dumped directory */
#define GNUTYPE_LONGLINK 'K'    /* long link name */
#define GNUTYPE_LONGNAME 'L'    /* long file name */
#define GNUTYPE_MULTIVOL 'M'    /* continuation of file from another volume */
#define GNUTYPE_NAMES    'N'    /* file name that does not fit into main hdr */
#define GNUTYPE_SPARSE   'S'    /* sparse file */
#define GNUTYPE_VOLHDR   'V'    /* tape/volume header */



/* help functions */

unsigned long getoct(char *p, int width)
{
  unsigned long result = 0;
  char c;

  while (width --)
  {
      c = *p++;
      if (c == ' ') /* ignore padding */
          continue;
      if (c == 0)   /* ignore padding, but also marks end of string */
          break;
      if (c < '0' || c > '7')
          return result; /* really an error, but we just ignore invalid values */
      result = result * 8 + (c - '0');
  }
  return result;
}

/* regular expression matching */

#define ISSPECIAL(c) (((c) == '*') || ((c) == '/'))

int ExprMatch(char *string,char *expr)
{
  while (1)
    {
      if (ISSPECIAL(*expr))
        {
          if (*expr == '/')
            {
              if (*string != '\\' && *string != '/')
                return 0;
              string ++; expr++;
            }
          else if (*expr == '*')
            {
              if (*expr ++ == 0)
                return 1;
              while (*++string != *expr)
                if (*string == 0)
                  return 0;
            }
        }
      else
        {
          if (*string != *expr)
            return 0;
          if (*expr++ == 0)
            return 1;
          string++;
        }
    }
}


/* returns 0 on failed checksum, nonzero if probably ok
   it was noted that some versions of tar compute
   signed chksums, though unsigned appears to be the
   standard; chksum is simple sum of all bytes in header
   as integers (using at least 17 bits) with chksum
   values treated as ASCII spaces.
*/
int valid_checksum(struct tar_header *header)
{
    unsigned hdrchksum = (unsigned) getoct(header->chksum, 8);
    signed schksum = 0;
    unsigned uchksum = 0;
    int i;

    for (i = 0; i < sizeof(struct tar_header); i++)
    {
        unsigned char val = ((unsigned char *)header)[i];
        if ((i >= 148) && (i < 156)) /* chksum */
        {
            val = ' ';
        }
        schksum += (signed char)val;
        uchksum += val;
    }
    printf("chksum: %d %d %d\n", hdrchksum, schksum, uchksum);

    if (hdrchksum == uchksum) return 1;
    if ((int)hdrchksum == schksum) return 2;
    return 0;
}


/* combines elements from tar header to produce
 * full [long] filename; prefix + [/] + name
 */
void getFullName(union tar_buffer *buffer, char *fname)
{
    int len = 0;

    /* NOTE: prepend buffer.head.prefix if tar archive expected to have it */
    if (*(buffer->header.prefix) && (*(buffer->header.prefix) != ' '))
    {
        /* copy over prefix */
        strncpy(fname,buffer->header.prefix, sizeof(buffer->header.prefix));
        fname[sizeof(buffer->header.prefix)-1] = '\0';
        /* ensure ends in dir separator, implied after if full prefix size used */
        len = strlen(fname)-1; /* assumed by test above at least 1 character */
        if ((fname[len]!='/') && (fname[len]!='\\'))
        {
            len++;
            fname[len]='/';
        }
        len++; /* index of 1st character after dir separator */
    }

    /* copy over filename portion */
    strncpy(fname+len,buffer->header.name, sizeof(buffer->header.name));
    fname[len+sizeof(buffer->header.name)] = '\0'; /* ensure terminated */
}


/* returns a pointer to a static buffer
 * containing fname after removing all but
 * path_sep_cnt path separators
 * if there are less than path_sep_cnt
 * separators then all will still be there.
 */
char * stripPath(int path_sep_cnt, char *fname)
{
  static char buffer[1024];
  char *fname_use = fname + strlen(fname);
  register int i=path_sep_cnt;
  do
  {
    if ( (*fname_use == '/') || (*fname_use == '\\') )
        {
      i--;
          if (i < 0) fname_use++;
          else fname_use--;
    }
        else
      fname_use--;
  } while ((i >= 0) && (fname_use > fname));

  strcpy(buffer, fname_use);
  return buffer;
}


gzFile infile;


/* Reads in a single TAR block
 */
long readBlock(int cm, void *buffer)
{
  long len = -1;

  len = gzread(infile, buffer, BLOCKSIZE);

  printf("len = %d\n", len);

  /* check for read errors and abort */
  if (len < 0)
  {
      printf("gzread: error decompressing\n");
      return -1;
  }
  /*
   * Always expect complete blocks to process
   * the tar information.
   */
  if (len != BLOCKSIZE)
  {
      printf("gzread: incomplete block read\n");
      return -1;
  }

  return len; /* success */
}


/* Tar file extraction
 * gzFile in, handle of input tarball opened with gzopen
 * int cm, compressionMethod
 * int junkPaths, nonzero indicates to ignore stored path (don't create directories)
 * enum KeepMode keep, indicates to perform if file exists
 * int iCnt, char *iList[], argv style list of files to extract, {0,NULL} for all
 * int xCnt, char *xList[], argv style list of files NOT to extract, {0,NULL} for none
 * int failOnHardLinks, if nonzero then will treat failure to create a hard link same as
 *   failure to create a regular file, 0 prints a warning if fails - note that hardlinks
 *   will always fail on Windows prior to NT 5 (Win 2000) or later and non NTFS file systems.
 *
 * returns 0 (or positive value) on success
 * returns negative value on error, where
 *   -1 means error reading from tarball
 *   -2 means error extracting file from tarball
 *   -3 means error creating hard link
 */
int tgz_extract(gzFile in, int cm)
{
  int           getheader = 1;    /* assume initial input has a tar header */

  union         tar_buffer buffer;
  unsigned long remaining;
  char          fname[BLOCKSIZE]; /* must be >= BLOCKSIZE bytes */
  time_t        tartime;

  /* do any prep work for extracting from compressed TAR file */
  infile = in;

  while (1)
  {
    if (readBlock(cm, &buffer) < 0) return -1;

    /*
     * If we have to get a tar header
     */
    if (getheader >= 1)
    {
      /*
       * if we met the end of the tar
       * or the end-of-tar block,
       * we are done
       */
      if (/* (len == 0)  || */ (buffer.header.name[0]== 0)) break;

      /* compute and check header checksum, support signed or unsigned */
      if (!valid_checksum(&(buffer.header)))
      {
          printf("tgz_extract: bad header checksum\n");
          return -1;
      }

      /* store time, so we can set the timestamp on files */
      tartime = (time_t) getoct(buffer.header.mtime, 12);

      /* copy over filename chunk from header, avoiding overruns */
      if (getheader == 1) /* use normal (short or posix long) filename from header */
      {
        /* NOTE: prepends any prefix, including separator, and ensures terminated */
                getFullName(&buffer, fname);
      }
      else /* use (GNU) long filename that preceeded this header */
      {
#if 0
        /* if (strncmp(fname,buffer.header.name,SHORTNAMESIZE-1) != 0) */
        char fs[SHORTNAMESIZE];   /* force strings to same max len, then compare */
        lstrcpyn(fs, fname, SHORTNAMESIZE);
        fs[SHORTNAMESIZE-1] = '\0';
        buffer.header.name[SHORTNAMESIZE-1] = '\0';
        if (lstrcmp(fs, buffer.header.name) != 0)
        {
            printf("tgz_extract: mismatched long filename\n");
            return -1;
        }
#else
        printf("tgz_extract: using GNU long filename [%s]", fname);
#endif
      }
      printf("fname: '%s'\n", fname);
    }
  } /* while(1) */

  return 0;
}


int main()
{
    gzFile *x;

    x = gzopen("ncurses.tar.gz", "rb");
    tgz_extract(x, 0);
    gzclose(x);
    return 0;
}
