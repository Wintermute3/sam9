// ----------------------------------------------------------------------------
// sam9boot - Utility to simplify dealing with the SAM9 RomBOOT facility.
// Copyright (C) 2011 Michael E. Nagy
// ----------------------------------------------------------------------------
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------
//
// This program was developed and tested under Linux.  With minor changes it
// should be possible to modify it to work under Windows as well.  To build
// it using the GNU compiler:
//
//                         g++ sam9boot.c -o sam9boot
//
// ----------------------------------------------------------------------------

#define VERSION "1.01" // 02-Nov-2011

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>

// ----------------------------------------------------------------------------
//  Local types for conciseness.
// ----------------------------------------------------------------------------

typedef unsigned int  bit32; // unsigned 32-bit
typedef unsigned char byte ; // unsigned  8-bit

typedef FILE       *  fptr;
typedef       char *  cptr;
typedef const char * ccptr;
typedef       byte *  bptr;

// ----------------------------------------------------------------------------
//  Command-line parameter values.
// ----------------------------------------------------------------------------

static ccptr ParamPort      = "/dev/ttyUSB0";
static ccptr ParamFileName  = NULL;
static ccptr ParamAddrStart = "$300000";
static ccptr ParamAddrGo    = NULL;
static ccptr ParamBytes     = NULL;

static bit32 ValueAddrGo    = 0;
static bit32 ValueAddrStart = 0;
static bit32 ValueBytes     = 0;

static bool FlagReceive     = false;
static bool FlagDump        = false;
static bool FlagSend        = false;
static bool FlagCpu         = false;
static bool FlagVerify      = false;
static bool FlagQuiet       = false;
static bool FlagTrace       = false;
static bool FlagInteractive = false;

// ----------------------------------------------------------------------------
//  Is input available on either the console or the RomBOOT serial port?
// ----------------------------------------------------------------------------

static int FileInputAvailable( int FileNumber) {
  struct timeval tv;
  fd_set fds;
  tv.tv_sec = 0;
  tv.tv_usec = 4000; // make as small as possible while avoiding verification errors
  FD_ZERO( &fds);
  FD_SET( FileNumber, &fds);
  return select( FileNumber+1, &fds, NULL, NULL, &tv);
}

// ----------------------------------------------------------------------------
//  Return one character from either the console or the RomBOOT serial port.
// ----------------------------------------------------------------------------

static byte FileGetCharacter( int FileNumber) {
  int r;
  byte c;
  if ((r = read( FileNumber, &c, sizeof(c))) < 0) {
    return r;
  }
  return c;
}

// ----------------------------------------------------------------------------
//  Keep track of console and sam9 serial port file numbers and terminal
//  io settings, and recent number of characters received from sam9.
// ----------------------------------------------------------------------------

static int FileNumberConsole, FileNumberSam9;
static struct termios OriginalConsoleTermIOs;
static bit32 ResponseCount = 0;

// ----------------------------------------------------------------------------
//  Reset console to initial terminal io settings.
// ----------------------------------------------------------------------------

static void ConsoleResetRawMode( void) {
  tcsetattr( FileNumberConsole, TCSANOW, &OriginalConsoleTermIOs);
}

// ----------------------------------------------------------------------------
//  Set up the console for raw binary i/o.
// ----------------------------------------------------------------------------

static void ConsoleSetRawMode( void) {
  struct termios NewTermIOs;
  tcgetattr( FileNumberConsole, &OriginalConsoleTermIOs);
  memcpy( &NewTermIOs, &OriginalConsoleTermIOs, sizeof( NewTermIOs));
  cfmakeraw( &NewTermIOs);
  NewTermIOs.c_iflag |= BRKINT;
  tcsetattr( FileNumberConsole, TCSANOW, &NewTermIOs);
  atexit( ConsoleResetRawMode);
}

// ----------------------------------------------------------------------------
//  Catch any response from RomBOOT and display it unless the quiet flag is
//  set.  If the response is a valid hex number, return the value.
// ----------------------------------------------------------------------------

static bit32 GetResponse( int FileNumber, bool FlagTrace = true) {
  static char Response[32];
  int n = 0;
  bit32 Value = 0;
  ResponseCount = 0;
  while (FileInputAvailable( FileNumber) && (n < (sizeof( Response) - 2))) {
    Response[n++] = FileGetCharacter( FileNumber);
    ResponseCount++;
  }
  if (n) {
    int State = 0;
    Response[n] = 0;
    if (FlagTrace) {
      printf( "%s", Response);
    }
    for (int i = 0; i < n; i++) {
      char c = Response[i];
      switch (State) {
        case 0: if (c == '0') State = 1;                      break;
        case 1: if (c == 'x') State = 2;      else State = 0; break;
        case 2: sscanf( Response+i, "%X", &Value); State = 3; break;
  } } }
  return Value;
}

// ----------------------------------------------------------------------------
//  A primative pass-thru terminal emulator.  Set console to raw mode and set
//  up to restore original settings on program exit.  Local echo is also
//  implemented.  If a 'go' address is defined, execute it immediately after
//  terminal initialization.
// ----------------------------------------------------------------------------

static void TerminalEmulator( fptr FileHandleSam9) {
  byte Key = 0;
  printf( "\n[[ interactive terminal mode - <esc> or <ctrl-c> to exit%s ]]\n", ParamAddrGo ? ", <enter> or # to GO" : "");
  fflush( stdout);
  ConsoleSetRawMode();
  if (ParamAddrGo) {
    fprintf( FileHandleSam9, "#\n", ValueAddrGo);
    GetResponse( FileNumberSam9);
    fprintf( FileHandleSam9, "G%X", ValueAddrGo);
    fflush( FileHandleSam9);
    GetResponse( FileNumberSam9);
    printf( "G%X", ValueAddrGo);
    fflush( stdout);
  }
  do {
    if (FileInputAvailable( FileNumberConsole)) {
      Key = FileGetCharacter( FileNumberConsole);
      if (Key == 0x0d) {
        Key = '#'; // SAM-BA uses # as EOL character for some unknown reason
      }
      write( FileNumberSam9, &Key, sizeof( Key));
      if ((Key > 0x1f) && (Key < 0x7f)) {
        write( FileNumberConsole, &Key, sizeof( Key));
    } }
    while (FileInputAvailable( FileNumberSam9)) {
      Key = FileGetCharacter( FileNumberSam9);
      write( FileNumberConsole, &Key, sizeof( Key));
      Key = 0;
    }
  } while ((Key != 0x1b) && (Key != 0x03)); // escape or ctrl-c
  ConsoleResetRawMode();
  printf( "\n[[ exit terminal mode ]]\n");
}

// ----------------------------------------------------------------------------
//  Convert a string to a 32-bit unsigned value.  Prefixes of 0x or $ indicate
//  hex strings, otherwise decimal assumed.
// ----------------------------------------------------------------------------

static bit32 NumericValue( ccptr String) {
  bit32 Value = 0;
  while (String && (*String == ' ')) {
    String++;
  }
  if (String) {
    if (String[0] == '$') {
      sscanf( String+1, "%x", &Value);
    } else {
      if ((String[0] == '0') && strchr( "Xx", String[1])) {
        sscanf( String+2, "%x", &Value);
      } else {
        sscanf( String, "%d", &Value);
  } } }
  return Value;
}

// ----------------------------------------------------------------------------
//  Display usage information on console.
// ----------------------------------------------------------------------------

static void ShowHelp( ccptr ExecutableName) {
  printf( "\n");
  printf( "Utility to simplify dealing with the SAM9 RomBOOT facility via a serial interface.\n");
  printf( "\n");
  printf( "Usage:  %s\n", ExecutableName);
  printf( "           {-p=port}\n");
  printf( "              {-f=filename {-a=address} {-n=bytes {-r} {-d}} {-s}}\n");
  printf( "                  {-g{=address}} {-c} {-v} {-q} {-t} {-i}\n");
  printf( "\n");
  printf( "Where:\n");
  printf( "\n");
  printf( "   -p=port  . . . . . . . . port to communicate with RomBOOT (default /dev/ttyUSB0)\n");
  printf( "   -f=filename  . . . . . . filename (needed by -r and -s)\n");
  printf( "   -a=address . . . . . . . address (default 0x300000, used by -r, -d and -s)\n");
  printf( "   -n=bytes . . . . . . . . number of bytes (defaults to filesize for -s)\n");
  printf( "   -r . . . . . . . . . . . receive file (also specify -f, -a and -n)\n");
  printf( "   -d . . . . . . . . . . . dump memory (also specify -a and -n or -s)\n");
  printf( "   -s . . . . . . . . . . . send file (also specify -f and -a)\n");
  printf( "   -g{=address} . . . . . . address to jump to (default -a)\n");
  printf( "   -c . . . . . . . . . . . query cpu part id\n");
  printf( "   -v . . . . . . . . . . . verify memory against file (also specify -f)\n");
  printf( "   -q . . . . . . . . . . . quiet (no non-essential i/o or messages)\n");
  printf( "   -t . . . . . . . . . . . trace details of upload/verify activity\n");
  printf( "   -i . . . . . . . . . . . interactive (terminal) mode\n");
  printf( "\n");
  printf( "All parameters are additive.  Relative order only matters for -a and -g.  Numeric\n");
  printf( "values may be entered as decimal (no prefix) or as hex with either 0x or $ prefix.\n");
  printf( "Parameters -r and -s are mutually exclusive.  If -s is specified, the actual send\n");
  printf( "file size overrides -n.\n");
  printf( "\n");
}

// ----------------------------------------------------------------------------
//  Parse the command line and extract parameters.  See ShowHelp() above for
//  the list of valid parameters and co-dependencies.  Check for missing
//  parameters required by specified parameters.
// ----------------------------------------------------------------------------

static bool ParseParameters( int argc, ccptr argv[]) {
  for (int n = 1; n < argc; n++) {
    bool Success = true;
    ccptr x = argv[n];
    if (x[0] != '-') {
      Success = false;
    } else {
      switch( x[1]) {
        case 'p': case 'f': case 'a': case 'n': case 'g':
          if (strlen( x) > 3) {
            if (x[2] == '=') {
              switch( x[1]) { // parameters with arguments
                case 'p': ParamPort      = x+3; break;
                case 'f': ParamFileName  = x+3; break;
                case 'a': ParamAddrStart = x+3; break;
                case 'n': ParamBytes     = x+3; break;
                case 'g': ParamAddrGo    = x+3; break;
              }
            } else {
              Success = false;
            }
          } else {
            if ((x[1] == 'g') && (strlen( x) == 2)) {
              ParamAddrGo = ParamAddrStart;
            } else {
              Success = false;
          } }
          break;
        case 'r': case 'd': case 's': case 'c': case 'v': case 'q': case 't': case 'i':
          switch( x[1]) { // simple switch parameters
            case 'r': FlagReceive     = true; break;
            case 'd': FlagDump        = true; break;
            case 's': FlagSend        = true; break;
            case 'c': FlagCpu         = true; break;
            case 'v': FlagVerify      = true; break;
            case 'q': FlagQuiet       = true; break;
            case 't': FlagTrace       = true; break;
            case 'i': FlagInteractive = true; break;
            default:
              Success = false;
          }
          break;
        default:
          Success = false;
    } }
    if (Success == false) {
      printf( "*** Invalid parameter: '%s'\n", x);
      return false;
  } }
  if (strncmp( ParamPort, "/dev/", 5)) {
    printf( "*** Invalid parameter: '-p=%s'\n", ParamPort);
    return false;
  }
  if ((FlagReceive || FlagSend) && (ParamFileName == NULL)) {
    printf( "*** Parameters '-r' and '-s' require '-f'!\n");
    return false;
  }
  if ((FlagReceive || FlagDump) && (ParamBytes == NULL)) {
    printf( "*** Parameters '-r' and '-d' require '-n'!\n");
//    return false;
  }
  if (FlagReceive && FlagSend) {
    printf( "*** Parameters '-r' and '-s' may not both be specified!\n");
    return false;
  }
  if (ParamAddrStart) {
    ValueAddrStart = NumericValue( ParamAddrStart);
  }
  if (ParamAddrGo) {
    ValueAddrGo = NumericValue( ParamAddrGo);
  }
  if (ParamBytes) {
    ValueBytes = NumericValue( ParamBytes);
    if (ValueBytes == 0) {
      printf( "*** Invalid parameter: '-n=%s'\n", ParamBytes);
      return false;
  } }
  return true;
}

// ----------------------------------------------------------------------------
//  Load a sam9 memory image into a dynamically-allocated buffer.
// ----------------------------------------------------------------------------

static bit32 MemoryCount = 0;
static bptr MemoryBuffer = NULL;

static bool LoadMemory( fptr FileHandleSam9, bit32 StartAddress, bit32 Count) {
  if (MemoryBuffer = (bptr) calloc( Count, 1)) {
    bit32 Address = StartAddress, Length = 0, Value;
    bit32 Chunk = ValueBytes > 3 ? 4 : 1;
    while (Length < Count) {
      switch (Chunk) {
        case 1:
          fprintf( FileHandleSam9, "o%5.5X,1#\n", Address);
          if (FlagTrace) {
            printf( "o%5.5X,1#", Address);
          }
          break;
        case 4:
          fprintf( FileHandleSam9, "w%5.5X,4#\n", Address);
          if (FlagTrace) {
            printf( "w%5.5X,4#", Address);
          }
          break;
      }
      Value = GetResponse( FileNumberSam9, FlagTrace);
      if (ResponseCount) {
        for (int n = 0; n < Chunk; n++) {
          MemoryBuffer[MemoryCount++] = Value & 0xff;
          Value >>= 8;
        }
        if ((Length % 256) == 0) {
          printf( "Downloading memory from $%x (%d bytes)...\r", StartAddress, Length);
          fflush( stdout);
        }
        Address += Chunk;
        Length += Chunk;
        if (Count - Length < 4) {
          Chunk = 1;
        }
      } else {
        fprintf( stderr, "*** Failed to download memory from $%x (%d bytes, %d expected, target unresponsive)!\n", StartAddress, MemoryCount, Count);
        return false;
    } }
    if (MemoryCount == Count) {
      return true;
    }
    fprintf( stderr, "*** Failed to download memory from $%x (%d bytes, %d expected)!\n", StartAddress, MemoryCount, Count);
  } else {
    fprintf( stderr, "*** Failed to download memory from $%x (%d bytes, calloc error)!\n", StartAddress, Count);
  }
  return false;
}

// ----------------------------------------------------------------------------
//  Load a file image from disk into a dynamically-allocated buffer.
// ----------------------------------------------------------------------------

static bit32 FileCount = 0;
static bptr FileBuffer = NULL;

static bool LoadFile( ccptr FileName) {
  if (fptr f = fopen( FileName, "rb")) {
    bit32 FileBytes = 0;
    fseek( f, 0, SEEK_END);
    if (FileBytes = ftell( f)) {
      rewind( f);
      if (ValueBytes == 0) {
        ValueBytes = FileBytes;
      }
      if (FileBuffer = (bptr) calloc( ValueBytes, 1)) {
        if (fread( FileBuffer, 1, ValueBytes, f) == ValueBytes) {
          fclose( f);
          return true;
        }
        fprintf( stderr, "*** Failed to load file '%s' (%d bytes, read error)!\n", FileName, ValueBytes);
      } else {
        fprintf( stderr, "*** Failed to load file '%s' (%d bytes, calloc error)!\n", FileName, ValueBytes);
      }
    } else {
      fprintf( stderr, "*** Failed to load file '%s' (zero length)!\n", FileName);
    }
    fclose( f);
  } else {
    fprintf( stderr, "*** Failed to load file '%s' (open error)!\n", FileName);
  }
  return false;
}

// ----------------------------------------------------------------------------
//  Main application.
// ----------------------------------------------------------------------------

int main( int argc, ccptr argv[]) {
  printf( "\nSAM9 Boot Utility Version " VERSION "\n");
  bool Success = true;
  if (argc > 1) {
    if (ParseParameters( argc, argv)) {
      printf( "\n");
      if (fptr FileHandleSam9 = fopen( ParamPort, "a+b")) {
        FileNumberConsole = fileno( stdin);
        FileNumberSam9 = fileno( FileHandleSam9);
        fprintf( FileHandleSam9, "#\n");
        if (FlagQuiet == false) {
          printf( "#");
        }
        GetResponse( FileNumberSam9, FlagQuiet ? false : true);
        if (FlagQuiet == false) {
          fprintf( FileHandleSam9, "V#\n");
          printf( "V#");
          GetResponse( FileNumberSam9);
        }

        //-------
        //  cpu
        //-------

        if (FlagCpu) {
          fprintf( FileHandleSam9, "wfffff240,4#\n");
          printf( "wfffff240,4#");
          bit32 PartId = GetResponse( FileNumberSam9);
          if (ResponseCount) {
            printf( "PartId = $%8.8X\n", PartId);
          } else {
            fflush( stdout);
            fprintf( stderr, "\n*** Failed to get cpu type (target unresponsive)!");
            fflush( stderr);
            Success = false;
        } }
        GetResponse( FileNumberSam9);
        printf( "\n");

        //---------------------------------
        //  send/verify - load file image
        //---------------------------------

        if (Success && (FlagSend | FlagVerify)) {
          if (ParamFileName) {
            if (LoadFile( ParamFileName)) {
              printf( "Loaded file '%s' (%d bytes) from disk.\n", ParamFileName, ValueBytes);
            } else {
              Success = false;
            }
          } else {
            printf( "*** Parameters '-s' and '-v' require '-f'!\n");
            Success = false;
        } }

        //--------
        //  send
        //--------

        if (Success && FlagSend) {
          bit32 Address = ValueAddrStart, Length = 0;
          int Chunk = ValueBytes > 3 ? 4 : 1;
          while (Length < ValueBytes) {
            bit32 Value = 0;
            for (int i = 0; i < Chunk; i++) {
              Value |= FileBuffer[Length++] << (i*8);
            }
            switch (Chunk) {
              case 1:
                fprintf( FileHandleSam9, "O%5.5X,%2.2X#\n", Address, Value);
                if (FlagTrace) {
                  printf( "O%5.5X,%2.2X#", Address, Value);
                }
                break;
              case 4:
                fprintf( FileHandleSam9, "W%5.5X,%8.8X#\n", Address, Value);
                if (FlagTrace) {
                  printf( "W%5.5X,%8.8X#", Address, Value);
                }
                break;
            }
            GetResponse( FileNumberSam9, FlagTrace);
            Address += Chunk;
            if ((Length % 256) == 0) {
              printf( "Uploading file '%s' (%d bytes) to memory at $%x...\r", ParamFileName, Length, ValueAddrStart);
              fflush( stdout);
            }
            if (ValueBytes - Length < 4) {
              Chunk = 1;
            }
            Value = 0;
          }
          printf( "Uploaded file '%s' (%d bytes) to memory at $%x.    \n", ParamFileName, Length, ValueAddrStart);
        }

        //---------------------------------------
        //  verify/recv/dump - load image buffer
        //---------------------------------------

        if (Success && (FlagVerify | FlagReceive | FlagDump)) {
          if (ValueBytes) {
            if (LoadMemory( FileHandleSam9, ValueAddrStart, ValueBytes)) {
              printf( "Downloaded memory from $%x (%d bytes).\n", ValueAddrStart, ValueBytes);
            } else {
              Success = false;
            }
          } else {
            printf( "*** Parameter '-d' requires '-n'!\n");
            Success = false;
        } }

        //-------------------------------
        //  verify data in image buffer
        //-------------------------------
        if (Success && FlagVerify) {
          if (ValueBytes) {
            for (bit32 i = 0; Success && (i < ValueBytes); i++) {
              if (FileBuffer[i] != MemoryBuffer[i]) {
                fprintf( stderr, "*** Verify memory at $%x (%d bytes) error at offset %d!\n", ValueAddrStart, ValueBytes, i);
                Success = false;
            } }
            if (Success) {
              printf( "Verified memory at $%x (%d bytes).\n", ValueAddrStart, ValueBytes);
            }
          } else {
            printf( "*** Parameter '-v' requires '-n'!\n");
            Success = false;
        } } 

        //-----------------------------
        //  recv data in image buffer
        //-----------------------------

        if (Success && FlagReceive && MemoryCount) {
          if (fptr f = fopen( ParamFileName, "wb")) {
            if (fwrite( MemoryBuffer, 1, MemoryCount, f) == MemoryCount) {
              printf( "Wrote %d bytes to file '%s'.\n", MemoryCount, ParamFileName);
            } else {
              fprintf( stderr, "*** Error writing %d bytes to file '%s'!\n", MemoryCount, ParamFileName);
              Success = false;
            }
            fclose( f);
          } else {
            fprintf( stderr, "*** Unable to open file '%s' for write!\n", ParamFileName);
            Success = false;
        } }

        //-----------------------------
        //  dump data in image buffer
        //-----------------------------

        if (FlagDump && MemoryCount) {
          printf( "\n");
          bit32 Address = ValueAddrStart, Offset = 0;
          char Template[66];
          while (Offset < MemoryCount) {
            for (int i = 0; i < sizeof( Template); i++) {
              Template[i] = ' ';
            }
            Template[sizeof(Template)-1] = 0;
            for (int i = 0, j = 0, k = 49; (i < 16) && (Offset < MemoryCount); i++) {
              byte Value = MemoryBuffer[Offset++];
              char Temp[16];
              sprintf( Temp, "%2.2x", Value);
              Template[j++] = Temp[0];
              Template[j++] = Temp[1]; j++;
              Template[k++] = ((Value > 0x1f) && (Value < 0x7f)) ? Value : '.';
            }
            printf( "$%6.6x  %s\n", Address, Template);
            Address += 16;
        } }

        //---------------------------------------------
        //  interactive terminal mode w/optional 'go'
        //---------------------------------------------

        if (FlagInteractive) {
          TerminalEmulator( FileHandleSam9);
        } else {
          if (Success && ParamAddrGo) {
            fprintf( FileHandleSam9, "G%X#\n", ValueAddrGo);
            printf( "G%X#\n", ValueAddrGo);
            GetResponse( FileNumberSam9);
        } }
        fclose( FileHandleSam9);
        printf( "\n");
      } else {
        fprintf( stderr, "*** Unable to open device '%s' for i/o!\n", ParamPort);
    } }
  } else {
    ShowHelp( argv[0]);
  }
  if (Success) {
    printf( "Exit code 0 - success.\n\n");
    return 0;
  }
  printf( "*** Exit code 1 - failure!\n\n");
  return 1;
}

// ----------------------------------------------------------------------------
//  End
// ----------------------------------------------------------------------------

