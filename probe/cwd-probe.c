/*
 * cwd-probe.c -- why can a directory a child ran in not be removed?
 *
 * `test/10-api/spawn.c` makes a directory, launches a child in it and removes
 * it again. On Linux the removal works; under wine the directory is left
 * behind, empty and removable by hand afterwards. This splits the two possible
 * reasons -- that removing *any* directory fails here, or that it fails only
 * once a child has used it -- and reports the errno of each attempt, plus what
 * waiting changes.
 *
 *   x86_64-w64-mingw32-clang -o cwd-probe.exe cwd-probe.c
 *   wine cwd-probe.exe
 *
 * The answer is a platform behaviour rather than a bug in mulle-spawn, which is
 * why it lives here and not in a test.
 */
#include <windows.h>

#include <direct.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>


static wchar_t   *to_wide( const char *s)
{
   size_t    n;
   wchar_t   *w;

   n = (size_t) MultiByteToWideChar( CP_UTF8, 0, s, -1, NULL, 0);
   w = malloc( n * sizeof( wchar_t));
   if( ! w)
      return( NULL);

   if( ! MultiByteToWideChar( CP_UTF8, 0, s, -1, w, (int) n))
   {
      free( w);
      return( NULL);
   }
   return( w);
}


static int   run_child_in( const char *self, const char *directory)
{
   PROCESS_INFORMATION   pi = { 0 };
   STARTUPINFOW          si = { 0 };
   wchar_t               *cmd;
   wchar_t               *wself;
   wchar_t               *wdir;
   size_t                n;

   wself = to_wide( self);
   wdir  = to_wide( directory);
   if( ! wself || ! wdir)
      return( 1);

   n   = wcslen( wself) + 16;
   cmd = malloc( n * sizeof( wchar_t));
   if( ! cmd)
      return( 1);

   wcscpy( cmd, L"\"");
   wcscat( cmd, wself);
   wcscat( cmd, L"\" child");

   si.cb = sizeof( si);

   if( ! CreateProcessW( NULL, cmd, NULL, NULL, TRUE, 0, NULL, wdir, &si, &pi))
   {
      printf( "  CreateProcessW failed: %lu\n", (unsigned long) GetLastError());
      return( 1);
   }

   WaitForSingleObject( pi.hProcess, INFINITE);
   printf( "  the child exited with %lu\n", (unsigned long) pi.dwProcessId);
   CloseHandle( pi.hThread);
   CloseHandle( pi.hProcess);
   return( 0);
}


static void   attempt( const char *label, const char *path)
{
   errno = 0;
   if( _rmdir( path))
      printf( "  %-24s failed, errno %d\n", label, errno);
   else
      printf( "  %-24s removed it\n", label);
}


int   main( int argc, char **argv)
{
   if( argc > 1)
   {
      printf( "child: running\n" );
      return( 0);
   }

   printf( "a directory nobody ran in:\n" );
   errno = 0;
   printf( "  _mkdir:                   %d (errno %d)\n", _mkdir( "probe-d1"), errno);
   attempt( "_rmdir right after:", "probe-d1");

   printf( "a directory a child ran in:\n" );
   errno = 0;
   printf( "  _mkdir:                   %d (errno %d)\n", _mkdir( "probe-d2"), errno);
   run_child_in( argv[ 0], "probe-d2");
   attempt( "_rmdir right after:", "probe-d2");
   Sleep( 500);
   attempt( "_rmdir after 500ms:", "probe-d2");
   Sleep( 2000);
   attempt( "_rmdir after 2.5s:", "probe-d2");
   attempt( "_rmdir again:", "probe-d2");

   return( 0);
}
