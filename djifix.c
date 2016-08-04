/**********
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**********/
/*
    A C program to repair corrupted video files that can sometimes be produced by
    DJI quadcopters.
    Version 2016-04-19

    Copyright (c) 2014-2016 Live Networks, Inc.  All rights reserved.

    Version history:
    - 2014-09-01: Initial version
    - 2014-09-21: When repairing 'type 2' files, we prompt the user to specify the video format
            that was used (because the SPS NAL unit - that we prepend to the repaired file -
	    differs for each video format).
    - 2014-10-04: We now automatically generate the name of the repaired file from the name of
            the original file (therefore, the user no longer specifies this on the command line).
	    Also, we can now handle certain (rare) files in which the
	    'ftyp','moov','mdat'(containing 'ftyp') sequence occurs more than once at the start.
    - 2014-10-11: When performing a 'type 2' repair, we now better handle the case where we see
            a 4-byte 'NAL size' of 0.  This anomalous situation can happen when a large chunk of
	    zero bytes appears in the file.  We now try to recover from this by scanning forward
	    until we see (what we hope to be) a 'NAL size' of 2, which would indicate
	    the resumption of sane data.
    - 2015-01-08: Handle anomalous 0xFFFFFFFF words that can appear at the start (or interior)
            of corrupted files.
    - 2015-01-24: We can now repair 'type 2' files that were recorded in 1080p/60 format.
            (The DJI Phantom 2 Vision+ doesn't yet record in this format, but some other cameras
	     can, and they can produce files that are corrupted in a similar way.)
	    We now also try to recover from encountering bad (far too large) NAL sizes when
	    repairing 'type 2' files, and unexpected garbage appearing at the beginning of files.
    - 2015-03-30: We now support two ('4k') video formats used by the Inspire 1:
            2160p/30 and 2160p/24, and updated the H.264 SPS data to work for 1080p/60 files
	    from the Inspire 1.  We also support a wider range of damaged files.
    - 2015-05-09: We now support an additional video format - 1080p/24 - used by the Inspire 1.
    - 2015-06-16: We now support an additional video format - 1080p/50 - used by the Inspire 1.
    - 2015-09-25: We now support two more video formats: 2160p/25 and 720p/25
    - 2015-11-03: We now support an additional video format - 1520p/30
    - 2015-11-27: Corrected(?) the SPS NAL unit prepended to each frame for the 2160p/25 format.
    - 2016-04-19: We now support an additional video format - 1520p/25
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(char const* progName) {
  fprintf(stderr, "Usage: %s name-of-video-file-to-repair\n", progName);
}

#define fourcc_ftyp (('f'<<24)|('t'<<16)|('y'<<8)|'p')
#define fourcc_moov (('m'<<24)|('o'<<16)|('o'<<8)|'v')
#define fourcc_free (('f'<<24)|('r'<<16)|('e'<<8)|'e')
#define fourcc_mdat (('m'<<24)|('d'<<16)|('a'<<8)|'t')

static int get1Byte(FILE* fid, unsigned char* result); /* forward */
static int get4Bytes(FILE* fid, unsigned* result); /* forward */
static int checkAtom(FILE* fid, unsigned fourccToCheck, unsigned* numRemainingBytesToSkip); /* forward */
static void doRepairType1(FILE* inputFID, FILE* outputFID, unsigned ftypSize); /* forward */
static void doRepairType2(FILE* inputFID, FILE* outputFID, unsigned second4Bytes); /* forward */

static char const* versionStr = "2016-04-19";
static char const* repairedFilenameStr = "-repaired";
static char const* startingToRepair = "Repairing the file (please wait)...";
static char const* cantRepair = "  We cannot repair this file!";

int main(int argc, char** argv) {
  char* inputFileName;
  char* outputFileName;
  FILE* inputFID;
  FILE* outputFID;
  unsigned numBytesToSkip, dummy;
  int repairType = 1; /* by default */
  unsigned repairType1FtypSize; /* used only for 'repair type 1' files */
  unsigned repairType2Second4Bytes; /* used only for 'repair type 2' files */

  do {
    fprintf(stderr, "%s, version %s; Copyright (c) 2014-2016 Live Networks, Inc. All rights reserved.\n", argv[0], versionStr);

    if (argc != 2) {
      usage(argv[0]);
      break;
    }
    inputFileName = argv[1];

    /* Open the input file: */
    inputFID  = fopen(inputFileName, "rb");
    if (inputFID == NULL) {
      perror("Failed to open file to repair");
      break;
    }

    /* Check the first 8 bytes of the file, to see whether the file starts with a 'ftyp' atom
       (repair type 1), or H.264 NAL units (repair type 2): */
    {
      unsigned first4Bytes, next4Bytes;
      int fileStartIsOK;
      int amAtStartOfFile = 1;

      if (!get4Bytes(inputFID, &first4Bytes) || !get4Bytes(inputFID, &next4Bytes)) {
	fprintf(stderr, "Unable to read the start of the file.%s\n", cantRepair);
	break;
      }

      fileStartIsOK = 1;
      while (1) {
	if (next4Bytes == fourcc_ftyp) {
	  /* Repair type 1 */
	  if (first4Bytes < 8 || fseek(inputFID, first4Bytes-8, SEEK_CUR) != 0) {
	    fprintf(stderr, "Bad length for initial 'ftyp' atom.%s\n", cantRepair);
	    fileStartIsOK = 0;
	  } else {
	    if (!amAtStartOfFile) fprintf(stderr, "Found 'ftyp' (at file position 0x%lx)\n", ftell(inputFID) - 8); else fprintf(stderr, "Saw initial 'ftyp'.\n");
	  }
	} else if (first4Bytes == 0x00000002) {
	  /* Assume repair type 2 */
	  if (!amAtStartOfFile) fprintf(stderr, "Found 0x00000002 (at file position 0x%lx)\n", ftell(inputFID) - 8);
	  repairType = 2;
	  repairType2Second4Bytes = next4Bytes;
	} else if (first4Bytes == 0x00000000 || first4Bytes == 0xFFFFFFFF) {
	  /* Skip initial 0x00000000 or 0xFFFFFFFF data at the start of the file: */
	  if (amAtStartOfFile) {
	    fprintf(stderr, "Skipping initial junk 0x%08X bytes at the start of the file...\n", first4Bytes);
	    amAtStartOfFile = 0;
	  }
	  first4Bytes = next4Bytes;
	  if (!get4Bytes(inputFID, &next4Bytes)) {
	    fprintf(stderr, "File appears to contain nothing but zeros or 0xFF!%s\n", cantRepair);
	    fileStartIsOK = 0;
	  } else {
	    continue;
	  }
	} else {
	  /* There's garbage at the beginning of the file.  Skip until we find sane data: */
	  unsigned char c;

	  if (amAtStartOfFile) {
	    fprintf(stderr, "Didn't see an initial 'ftyp' atom, or 0x00000002.  Looking for data that we understand...\n");
	    amAtStartOfFile = 0;
	  }
	  if (!get1Byte(inputFID, &c)) {
	    /* We reached the end of the file, without seeing any data that we understand! */
	    fprintf(stderr, "...Unable to find sane initial data.%s\n", cantRepair);
	    fileStartIsOK = 0;
	  } else {
	    /* Shift "first4Bytes" and "next4Bytes" 1-byte to the left, and keep trying: */
	    first4Bytes = ((first4Bytes<<8)&0xFFFFFF00) | ((next4Bytes>>24)&0x000000FF);
	    next4Bytes = ((next4Bytes<<8)&0xFFFFFF00) | c;
	    continue;
	  }
	}

	/* If we get here, we've found the initial data that we're looking for, or end-of-file: */
	break;
      }
      if (!fileStartIsOK) break; /* An error occurred at/near the start of the file */
    }

    if (repairType == 1) {
      /* Check for a 'moov' atom next: */
      if (checkAtom(inputFID, fourcc_moov, &numBytesToSkip)) {
	fprintf(stderr, "Saw 'moov' (size %d == 0x%08x).\n", 8+numBytesToSkip, 8+numBytesToSkip);
	if (fseek(inputFID, numBytesToSkip, SEEK_CUR) != 0) {
	  fprintf(stderr, "Input file was truncated before end of 'moov'.%s\n", cantRepair);
	  break;
	}
      } else {
	fprintf(stderr, "Didn't see a 'moov' atom.\n");
	/* It's possible that this was a 'mdat' atom instead.  Rewind, and check for that next: */
	if (fseek(inputFID, -8, SEEK_CUR) != 0) {
	  fprintf(stderr, "Failed to rewind 8 bytes.%s\n", cantRepair);
	  break;
	}
      }

      /* Check for a 'free' atom that sometimes appears before 'mdat': */
      if (checkAtom(inputFID, fourcc_free, &numBytesToSkip)) {
	fprintf(stderr, "Saw 'free' (size %d == 0x%08x).\n", 8+numBytesToSkip, 8+numBytesToSkip);
	if (fseek(inputFID, numBytesToSkip, SEEK_CUR) != 0) {
	  fprintf(stderr, "Input file was truncated before end of 'free'.%s\n", cantRepair);
	  break;
	}
      } else {
	// It wasn't 'free', so rewind over the header
	if (fseek(inputFID, -8, SEEK_CUR) != 0) {
	  fprintf(stderr, "Failed to rewind 8 bytes.%s\n", cantRepair);
	  break;
	}
      }

      /* Check for a 'mdat' atom next: */
      if (checkAtom(inputFID, fourcc_mdat, &dummy)) {
	fprintf(stderr, "Saw 'mdat'.\n");

	/* Check whether the 'mdat' data begins with a 'ftyp' atom: */
	if (checkAtom(inputFID, fourcc_ftyp, &numBytesToSkip)) {
	  /* On rare occasions, this situation is repeated: The remainder of the file consists
	     of 'ftyp', 'moov', 'mdat' - with the 'mdat' data beginning with 'ftyp' again.
	     Check for this now:
	  */
	  long curPos;

	  while (1) {
	    unsigned nbts_moov;

	    curPos = ftell(inputFID); /* remember where we are now */
	    if (fseek(inputFID, numBytesToSkip, SEEK_CUR) != 0) break;
	    if (!checkAtom(inputFID, fourcc_moov, &nbts_moov)) break;
	    if (fseek(inputFID, nbts_moov, SEEK_CUR) != 0) break;
	    if (!checkAtom(inputFID, fourcc_mdat, &dummy)) break; /* can 0x0000002 ever occur? */
	    if (!checkAtom(inputFID, fourcc_ftyp, &numBytesToSkip)) break;
	    fprintf(stderr, "(Saw nested 'ftyp' within 'mdat')\n");
	  }
	  fseek(inputFID, curPos, SEEK_SET); /* restore our old position */

	  repairType1FtypSize = numBytesToSkip+8;
	  fprintf(stderr, "Saw a 'ftyp' within the 'mdat' data.  We can repair this file.\n");
	} else {
	  fprintf(stderr, "Didn't see a 'ftyp' atom inside the 'mdat' data.\n");
	  /* It's possible that the 'mdat' data began with 0x00000002 (i.e., a 'type 2' repair).*/
	  /* Rewind, and check for that next: */
	  if (fseek(inputFID, -8, SEEK_CUR) != 0) {
	    fprintf(stderr, "Failed to rewind 8 bytes.%s\n", cantRepair);
	    break;
	  }
	  repairType = 2;
	}
      } else {
	fprintf(stderr, "Didn't see a 'mdat' atom.\n");
	/* It's possible that the remaining bytes begin with 0x00000002 (i.e., a 'type 2' repair).*/
	/* Check for that next: */
	repairType = 2;
      }

      if (repairType == 2) {
	/* Check for 0x00000002 occurring next, or nearby, at a 4-byte boundary: */
	unsigned first4Bytes, next4Bytes;
	int saw2 = 0;

	fprintf(stderr, "Looking for 0x00000002...\n");
	while (1) {
	  if (!get4Bytes(inputFID, &first4Bytes) || !get4Bytes(inputFID, &next4Bytes)) break;/*eof*/
	  if (first4Bytes == 0x00000002) {
	    saw2 = 1;
	    fprintf(stderr, "Found 0x00000002 (at file position 0x%lx)\n", ftell(inputFID) - 8);
	    repairType2Second4Bytes = next4Bytes;
	    break;
	  } else {
	    first4Bytes = next4Bytes;
	    if (!get4Bytes(inputFID, &next4Bytes)) break;/*eof*/
	  }
	}

	if (!saw2) {
	  /* OK, now we have to give up: */
	  fprintf(stderr, "Didn't see 0x00000002.%s\n", cantRepair);
	  break;
	}
      }
    }

    if (repairType == 2) {
      fprintf(stderr, "We can repair this file, but the result will be a '.h264' file (playable by the VLC media player), not a '.mp4' file.\n");
    }

    /* Now generate the output file name, and open the output file: */
    {
      unsigned suffixLen, outputFileNameSize;
      char* dotPtr = strrchr(inputFileName, '.');
      if (dotPtr == NULL) {
	dotPtr = &inputFileName[strlen(inputFileName)];
      }
      *dotPtr = '\0';

      suffixLen = repairType == 1 ? 3/*mp4*/ : 4/*h264*/;
      outputFileNameSize = (dotPtr - inputFileName) + 1/*dot*/ + suffixLen + 1/*trailing '\0'*/;
      outputFileName = malloc(outputFileNameSize);
      sprintf(outputFileName, "%s%s.%s", inputFileName, repairedFilenameStr,
	      repairType == 1 ? "mp4" : "h264");

      outputFID = fopen(outputFileName, "wb");
      if (outputFID == NULL) {
	perror("Failed to open output file");
	free(outputFileName);
	break;
      }
    }

    /* Begin the repair: */
    if (repairType == 1) {
      doRepairType1(inputFID, outputFID, repairType1FtypSize);
    } else { /* repairType == 2 */
      doRepairType2(inputFID, outputFID, repairType2Second4Bytes);
    }

    fprintf(stderr, "...done\n");
    fclose(outputFID);
    fprintf(stderr, "\nRepaired file is \"%s\"\n", outputFileName);
    free(outputFileName);

    if (repairType == 2) {
      fprintf(stderr, "This file can be played by the VLC media player (available at <http://www.videolan.org/vlc/>)\n");

      /* Check whether the output file name ends with ".h264" (or ".H264").  If it doesn't,
	 warn the user that he needs to change the name in order for the file to be playable.
      */
      {
	size_t outputFileNameLen = strlen(outputFileName);
	if (outputFileNameLen < 5 ||
	    (strcmp(&outputFileName[outputFileNameLen-5], ".h264") != 0 &&
	     strcmp(&outputFileName[outputFileNameLen-5], ".H264") != 0)) {
	  fprintf(stderr, "but you MUST first rename the file so that its name ends with \".h264\"!\n");
	}
      }
    }

    /* OK */
    return 0;
  } while (0);

  /* An error occurred: */
  return 1;
}

static int get1Byte(FILE* fid, unsigned char* result) {
  int fgetcResult;

  fgetcResult = fgetc(fid);
  if (feof(fid) || ferror(fid)) return 0;

  *result = (unsigned char)fgetcResult;
  return 1;
}

static int get4Bytes(FILE* fid, unsigned* result) {
  unsigned char c1, c2, c3, c4;

  if (!get1Byte(fid, &c1)) return 0;
  if (!get1Byte(fid, &c2)) return 0;
  if (!get1Byte(fid, &c3)) return 0;
  if (!get1Byte(fid, &c4)) return 0;

  *result = (c1<<24)|(c2<<16)|(c3<<8)|c4;
  return 1;
}

static int checkAtom(FILE* fid, unsigned fourccToCheck, unsigned* numRemainingBytesToSkip) {
  do {
    unsigned atomSize, fourcc;

    if (!get4Bytes(fid, &atomSize)) break;

    if (!get4Bytes(fid, &fourcc) || fourcc != fourccToCheck) break;

    if (atomSize < 8) break; /* atom size should be >= 8 */
    *numRemainingBytesToSkip = atomSize - 8;

    return 1;
  } while (0);

  /* An error occurred: */
  return 0;
}

static void doRepairType1(FILE* inputFID, FILE* outputFID, unsigned ftypSize) {
  fprintf(stderr, "%s", startingToRepair);

  /* Begin the repair by writing the header for the initial 'ftype' atom: */
  fputc(ftypSize>>24, outputFID);
  fputc(ftypSize>>16, outputFID);
  fputc(ftypSize>>8, outputFID);
  fputc(ftypSize, outputFID);

  fputc('f', outputFID); fputc('t', outputFID); fputc('y', outputFID); fputc('p', outputFID);

  /* Then complete the repair by copying from the input file to the output file: */
  {
    unsigned char c;

    while (get1Byte(inputFID, &c)) fputc(c, outputFID);
  }
}

#define wr(c) fputc((c), outputFID)

static void putStartCode(FILE* outputFID) {
  wr(0x00); wr(0x00); wr(0x00); wr(0x01);
}

static unsigned char SPS_2160p30[] = { 0x27, 0x64, 0x00, 0x33, 0xac, 0x34, 0xc8, 0x03, 0xc0, 0x04, 0x3e, 0xc0, 0x5a, 0x80, 0x80, 0x80, 0xa0, 0x00, 0x00, 0x7d, 0x20, 0x00, 0x1d, 0x4c, 0x1d, 0x0c, 0x00, 0x07, 0x27, 0x08, 0x00, 0x01, 0xc9, 0xc3, 0x97, 0x79, 0x71, 0xa1, 0x80, 0x00, 0xe4, 0xe1, 0x00, 0x00, 0x39, 0x38, 0x72, 0xef, 0x2e, 0x1f, 0x08, 0x84, 0x53, 0x80, 0xff };
// The following was used in an earlier version of the software, but does not appear to be correct:
//static unsigned char SPS_2160p25[] = { 0x27, 0x64, 0x00, 0x33, 0xac, 0x34, 0xc8, 0x01, 0x00, 0x01, 0x0f, 0xb0, 0x16, 0xa0, 0x20, 0x20, 0x28, 0x00, 0x00, 0x1f, 0x40, 0x00, 0x06, 0x1a, 0x87, 0x43, 0x00, 0x01, 0xc9, 0xc2, 0x00, 0x00, 0x72, 0x70, 0xe5, 0xde, 0x5c, 0x68, 0x60, 0x00, 0x39, 0x38, 0x40, 0x00, 0x0e, 0x4e, 0x1c, 0xbb, 0xcb, 0x87, 0xc2, 0x21, 0x14, 0xe0, 0xff };
static unsigned char SPS_2160p25[] = { 0x27, 0x64, 0x00, 0x33, 0xac, 0x34, 0xc8, 0x03, 0xc0, 0x04, 0x3e, 0xc0, 0x5a, 0x80, 0x80, 0x80, 0xa0, 0x00, 0x00, 0x7d, 0x00, 0x00, 0x18, 0x6a, 0x1d, 0x0c, 0x00, 0x07, 0x27, 0x08, 0x00, 0x01, 0xc9, 0xc3, 0x97, 0x79, 0x71, 0xa1, 0x80, 0x00, 0xe4, 0xe1, 0x00, 0x00, 0x39, 0x38, 0x72, 0xef, 0x2e, 0x1f, 0x08, 0x84, 0x53, 0x80, 0xff };
static unsigned char SPS_2160p24[] = { 0x27, 0x64, 0x00, 0x33, 0xac, 0x34, 0xc8, 0x01, 0x00, 0x01, 0x0f, 0xb0, 0x16, 0xa0, 0x20, 0x20, 0x28, 0x00, 0x00, 0x1f, 0x48, 0x00, 0x05, 0xdc, 0x07, 0x43, 0x00, 0x01, 0xc9, 0xc2, 0x00, 0x00, 0x72, 0x70, 0xe5, 0xde, 0x5c, 0x68, 0x60, 0x00, 0x39, 0x38, 0x40, 0x00, 0x0e, 0x4e, 0x1c, 0xbb, 0xff };
static unsigned char SPS_1520p30[] = { 0x27, 0x64, 0x00, 0x29, 0xac, 0x34, 0xc8, 0x02, 0xa4, 0x0b, 0xfb, 0x01, 0x6a, 0x02, 0x02, 0x02, 0x80, 0x00, 0x01, 0xf4, 0x80, 0x00, 0x75, 0x30, 0x74, 0x30, 0x00, 0x13, 0x12, 0xc0, 0x00, 0x04, 0xc4, 0xb4, 0x5d, 0xe5, 0xc6, 0x86, 0x00, 0x02, 0x62, 0x58, 0x00, 0x00, 0x98, 0x96, 0x8b, 0xbc, 0xb8, 0x7c, 0x22, 0x11, 0x4e, 0x00, 0x00, 0x00, 0xff };
static unsigned char SPS_1520p25[] = { 0x27, 0x64, 0x00, 0x29, 0xac, 0x34, 0xc8, 0x02, 0xa4, 0x0b, 0xfb, 0x01, 0x6a, 0x02, 0x02, 0x02, 0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x19, 0x74, 0x30, 0x00, 0x13, 0x12, 0xc0, 0x00, 0x04, 0xc4, 0xb4, 0x5d, 0xe5, 0xc6, 0x86, 0x00, 0x02, 0x62, 0x58, 0x00, 0x00, 0x98, 0x96, 0x8b, 0xbc, 0xb8, 0x7c, 0x22, 0x11, 0x4e, 0xff };
static unsigned char SPS_1080p60[] = { 0x27, 0x64, 0x00, 0x2a, 0xac, 0x34, 0xc8, 0x07, 0x80, 0x22, 0x7e, 0x5c, 0x05, 0xa8, 0x08, 0x08, 0x0a, 0x00, 0x00, 0x07, 0xd2, 0x00, 0x03, 0xa9, 0x81, 0xd0, 0xc0, 0x00, 0x4c, 0x4b, 0x00, 0x00, 0x13, 0x12, 0xd1, 0x77, 0x97, 0x1a, 0x18, 0x00, 0x09, 0x89, 0x60, 0x00, 0x02, 0x62, 0x5a, 0x2e, 0xf2, 0xe1, 0xf0, 0x88, 0x45, 0x16, 0xff };
static unsigned char SPS_1080i60[] = { 0x27, 0x4d, 0x00, 0x2a, 0x9a, 0x66, 0x03, 0xc0, 0x22, 0x3e, 0xf0, 0x16, 0xc8, 0x00, 0x00, 0x1f, 0x48, 0x00, 0x07, 0x53, 0x07, 0x43, 0x00, 0x02, 0x36, 0x78, 0x00, 0x02, 0x36, 0x78, 0x5d, 0xe5, 0xc6, 0x86, 0x00, 0x04, 0x6c, 0xf0, 0x00, 0x04, 0x6c, 0xf0, 0xbb, 0xcb, 0x87, 0xc2, 0x21, 0x14, 0x58, 0xff };
static unsigned char SPS_1080p50[] = { 0x27, 0x64, 0x00, 0x29, 0xac, 0x34, 0xc8, 0x07, 0x80, 0x22, 0x7e, 0x5c, 0x05, 0xa8, 0x08, 0x08, 0x0a, 0x00, 0x00, 0x07, 0xd0, 0x00, 0x03, 0x0d, 0x41, 0xd0, 0xc0, 0x00, 0x4c, 0x4b, 0x00, 0x00, 0x13, 0x12, 0xd1, 0x77, 0x97, 0x1a, 0x18, 0x00, 0x09, 0x89, 0x60, 0x00, 0x02, 0x62, 0x5a, 0x2e, 0xf2, 0xe1, 0xf0, 0x88, 0x45, 0x16, 0xff };
static unsigned char SPS_1080p30[] = { 0x27, 0x4d, 0x00, 0x28, 0x9a, 0x66, 0x03, 0xc0, 0x11, 0x3f, 0x2e, 0x02, 0xd9, 0x00, 0x00, 0x03, 0x03, 0xe9, 0x00, 0x00, 0xea, 0x60, 0xe8, 0x60, 0x00, 0xe2, 0x98, 0x00, 0x03, 0x8a, 0x60, 0xbb, 0xcb, 0x8d, 0x0c, 0x00, 0x1c, 0x53, 0x00, 0x00, 0x71, 0x4c, 0x17, 0x79, 0x70, 0xf8, 0x44, 0x22, 0x8b, 0xff };
static unsigned char SPS_1080p25[] = { 0x27, 0x4d, 0x00, 0x28, 0x9a, 0x66, 0x03, 0xc0, 0x11, 0x3f, 0x2e, 0x02, 0xd9, 0x00, 0x00, 0x03, 0x03, 0xe8, 0x00, 0x00, 0xc3, 0x50, 0xe8, 0x60, 0x00, 0xdc, 0xf0, 0x00, 0x03, 0x73, 0xb8, 0xbb, 0xcb, 0x8d, 0x0c, 0x00, 0x1b, 0x9e, 0x00, 0x00, 0x6e, 0x77, 0x17, 0x79, 0x70, 0xf8, 0x44, 0x22, 0x8b, 0xff };
static unsigned char SPS_1080p24[] = { 0x27, 0x64, 0x00, 0x29, 0xac, 0x34, 0xc8, 0x07, 0x80, 0x22, 0x7e, 0x5c, 0x05, 0xa8, 0x08, 0x08, 0x0a, 0x00, 0x00, 0x07, 0xd2, 0x00, 0x01, 0x77, 0x01, 0xd0, 0xc0, 0x00, 0xbe, 0xbc, 0x00, 0x00, 0xbe, 0xbc, 0x17, 0x79, 0x71, 0xa1, 0x80, 0x01, 0x7d, 0x78, 0x00, 0x01, 0x7d, 0x78, 0x2e, 0xf2, 0xe1, 0xf0, 0x88, 0x45, 0x16, 0x00, 0x00, 0x00, 0xff };
static unsigned char SPS_720p60[] = { 0x27, 0x4d, 0x00, 0x20, 0x9a, 0x66, 0x02, 0x80, 0x2d, 0xd8, 0x0b, 0x64, 0x00, 0x00, 0x0f, 0xa4, 0x00, 0x07, 0x53, 0x03, 0xa1, 0x80, 0x03, 0x8a, 0x60, 0x00, 0x0e, 0x29, 0x82, 0xef, 0x2e, 0x34, 0x30, 0x00, 0x71, 0x4c, 0x00, 0x01, 0xc5, 0x30, 0x5d, 0xe5, 0xc3, 0xe1, 0x10, 0x8a, 0x34, 0xff };
static unsigned char SPS_720p30[] = { 0x27, 0x4d, 0x00, 0x1f, 0x9a, 0x66, 0x02, 0x80, 0x2d, 0xd8, 0x0b, 0x64, 0x00, 0x00, 0x0f, 0xa4, 0x00, 0x03, 0xa9, 0x83, 0xa1, 0x80, 0x02, 0x5c, 0x40, 0x00, 0x09, 0x71, 0x02, 0xef, 0x2e, 0x34, 0x30, 0x00, 0x4b, 0x88, 0x00, 0x01, 0x2e, 0x20, 0x5d, 0xe5, 0xc3, 0xe1, 0x10, 0x8a, 0x34, 0xff };
static unsigned char SPS_720p25[] = { 0x27, 0x64, 0x00, 0x28, 0xac, 0x34, 0xc8, 0x05, 0x00, 0x5b, 0xb0, 0x16, 0xa0, 0x20, 0x20, 0x28, 0x00, 0x00, 0x1f, 0x40, 0x00, 0x06, 0x1a, 0x87, 0x43, 0x00, 0x0f, 0xd4, 0x80, 0x00, 0xfd, 0x4b, 0x5d, 0xe5, 0xc6, 0x86, 0x00, 0x1f, 0xa9, 0x00, 0x01, 0xfa, 0x96, 0xbb, 0xcb, 0x87, 0xc2, 0x21, 0x14, 0x78, 0xff };
static unsigned char SPS_480p30[] = { 0x27, 0x4d, 0x40, 0x1e, 0x9a, 0x66, 0x05, 0x01, 0xed, 0x80, 0xb6, 0x40, 0x00, 0x00, 0xfa, 0x40, 0x00, 0x3a, 0x98, 0x3a, 0x10, 0x00, 0x5e, 0x68, 0x00, 0x02, 0xf3, 0x40, 0xbb, 0xcb, 0x8d, 0x08, 0x00, 0x2f, 0x34, 0x00, 0x01, 0x79, 0xa0, 0x5d, 0xe5, 0xc3, 0xe1, 0x10, 0x8a, 0x3c, 0xff };

static unsigned char PPS_P2VP[] =    { 0x28, 0xee, 0x3c, 0x80, 0xff };
static unsigned char PPS_Inspire[] = { 0x28, 0xee, 0x38, 0x30, 0xff };

static void doRepairType2(FILE* inputFID, FILE* outputFID, unsigned second4Bytes) {
  /* Begin the repair by writing SPS and PPS NAL units (each preceded by a 'start code'): */
  {
    int formatCode;
    unsigned char* sps;
    unsigned char* pps;
    unsigned char c;

    /* The content of the SPS NAL unit depends upon which video format was used.
       Prompt the user for this now:
    */
    while (1) {
      fprintf(stderr, "First, however, we need to know which video format was used.  Enter this now.\n");
      fprintf(stderr, "\tIf the video format was 2160p(4k), 30fps: Type 0, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 2160p(4k), 25fps: Type 1, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 2160p(4k), 24fps: Type 2, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 1520p, 30fps: Type 3, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 1520p, 25fps: Type 4, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 1080p, 60fps: Type 5, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 1080i, 60fps: Type 6, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 1080p, 50fps: Type 7, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 1080p, 30fps: Type 8, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 1080p, 25fps: Type 9, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 1080p, 24fps: Type A, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 720p, 60fps: Type B, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 720p, 30fps: Type C, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 720p, 25fps: Type D, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf the video format was 480p, 30fps: Type E, then the \"Return\" key.\n");
      fprintf(stderr, "(If you are unsure which video format was used, then guess as follows:\n");
      fprintf(stderr, "\tIf your file was from a Phantom 2 Vision+: Type 8, then the \"Return\" key.\n");
      fprintf(stderr, "\tIf your file was from an Inspire: Type 2, then the \"Return\" key.\n");
      fprintf(stderr, " If the resulting file is unplayable by VLC, then you probably guessed the wrong format;\n");
      fprintf(stderr, " try again with another format.)\n");
      do {formatCode = getchar(); } while (formatCode == '\r' && formatCode == '\n');
      if ((formatCode >= '0' && formatCode <= '9') ||
	  (formatCode >= 'a' && formatCode <= 'e') ||
	  (formatCode >= 'A' && formatCode <= 'E')) {
	break;
      }
      fprintf(stderr, "Invalid entry!\n");
    }

    fprintf(stderr, "%s", startingToRepair);
    switch (formatCode) {
      case '0': { sps = SPS_2160p30; pps = PPS_Inspire; break; }
      case '1': { sps = SPS_2160p25; pps = PPS_Inspire; break; }
      case '2': { sps = SPS_2160p24; pps = PPS_Inspire; break; }
      case '3': { sps = SPS_1520p30; pps = PPS_Inspire; break; }
      case '4': { sps = SPS_1520p25; pps = PPS_Inspire; break; }
      case '5': { sps = SPS_1080p60; pps = PPS_Inspire; break; }
      case '6': { sps = SPS_1080i60; pps = PPS_P2VP; break; }
      case '7': { sps = SPS_1080p50; pps = PPS_Inspire; break; }
      case '8': { sps = SPS_1080p30; pps = PPS_P2VP; break; }
      case '9': { sps = SPS_1080p25; pps = PPS_P2VP; break; }
      case 'a': case 'A': { sps = SPS_1080p24; pps = PPS_Inspire; break; }
      case 'b': case 'B': { sps = SPS_720p60; pps = PPS_P2VP; break; }
      case 'c': case 'C': { sps = SPS_720p30; pps = PPS_P2VP; break; }
      case 'd': case 'D': { sps = SPS_720p25; pps = PPS_Inspire; break; }
      case 'e': case 'E': { sps = SPS_480p30; pps = PPS_P2VP; break; }
      default: { sps = SPS_1080p30; pps = PPS_P2VP; break; } /* shouldn't happen */
    };

    /*SPS*/
    putStartCode(outputFID);
    while ((c = *sps++) != 0xff) wr(c);

    /*PPS*/
    putStartCode(outputFID);
    while ((c = *pps++) != 0xff) wr(c);
  }

  /* Then write the first (2-byte) NAL unit, preceded by a 'start code': */
  putStartCode(outputFID);
  wr(second4Bytes>>24); wr(second4Bytes>>16);

  /* Then repeatedly:
     1/ Read a 4-byte NAL unit size.
     2/ Write a 'start code'.
     3/ Read 'NAL unit size' bytes, and write them to the output file.
  */
  {
    unsigned nalSize;
    unsigned char c1, c2;

    if (!get1Byte(inputFID, &c1)) return;
    if (!get1Byte(inputFID, &c2)) return;
    nalSize = ((second4Bytes&0xFFFF)<<16)|(c1<<8)|c2; /* for the first NAL unit */

    while (!feof(inputFID)) {
      putStartCode(outputFID);
      while (nalSize-- > 0) {
	wr(fgetc(inputFID));
      }

      if (!get4Bytes(inputFID, &nalSize)) return;
      if (nalSize == 0 || nalSize > 0x00FFFFFF) {
	/* An anomalous situation.  Try to recover from this by repeatedly reading bytes until
	   we get a 'nalSize' of 0x00000002.  With luck, that will begin sane data once again.
	*/
	unsigned char c;

	fprintf(stderr, "\n(Skipping over anomalous bytes...");
	do {
	  if (!get1Byte(inputFID, &c)) return;
	  nalSize = (nalSize<<8)|c;
	} while (nalSize != 2);
	fprintf(stderr, "...done)\nContinuing to repair the file (please wait)...");
      }
    }
  }
}
