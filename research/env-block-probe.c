/*
 * env-block-probe.c -- what does CreateProcessW actually do with an
 * environment block?
 *
 * mulle-spawn builds one for `mulle_spawn_open_with_options` and a child
 * launched with it reported nothing at all, which is the same as being handed
 * an empty environment. This splits that in two: first it prints the block it
 * built, then it launches itself with that block so the child can say what it
 * saw. Run it with no arguments; the child is the same program with one.
 *
 *   x86_64-w64-mingw32-clang -o env-block-probe.exe env-block-probe.c
 *   wine env-block-probe.exe
 *
 * Not a test, and not built by anything: it is here because the answer turned
 * out to be a missing flag rather than a platform behaviour -- and because the
 * first reading of it, without that flag, looked conclusive and was wrong. The
 * next person to doubt the environment block should not have to rediscover
 * which half is which.
 */
#include <windows.h>

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


static wchar_t   *build_block( char * const *env, size_t *capacity_out)
{
   size_t    offset;
   size_t    capacity;
   wchar_t   *block;
   int       count;
   int       i;
   int       n;

   capacity = 2;
   for( count = 0; env[ count]; count++)
      capacity += (size_t) MultiByteToWideChar( CP_UTF8, 0, env[ count], -1, NULL, 0);

   *capacity_out = capacity;

   block = malloc( capacity * sizeof( wchar_t));
   if( ! block)
      return( NULL);

   offset = 0;
   for( i = 0; i < count; i++)
   {
      n = MultiByteToWideChar( CP_UTF8, 0, env[ i], -1, &block[ offset],
                               (int) (capacity - offset));
      if( ! n)
      {
         free( block);
         return( NULL);
      }
      offset += (size_t) n;
   }

   block[ offset]     = 0;
   block[ offset + 1] = 0;
   return( block);
}


static int   run_child( const char *self, void *environment, DWORD flags)
{
   PROCESS_INFORMATION   pi = { 0 };
   STARTUPINFOW          si = { 0 };
   wchar_t               *cmd;
   wchar_t               *wself;
   size_t                n;
   DWORD                 error;

   wself = to_wide( self);
   if( ! wself)
      return( 1);

   n   = wcslen( wself) + 16;
   cmd = malloc( n * sizeof( wchar_t));
   if( ! cmd)
      return( 1);

   wcscpy( cmd, L"\"");
   wcscat( cmd, wself);
   wcscat( cmd, L"\" child");

   si.cb = sizeof( si);

   if( ! CreateProcessW( NULL, cmd, NULL, NULL, TRUE, flags, environment, NULL, &si, &pi))
   {
      error = GetLastError();
      printf( "CreateProcessW failed: %lu\n", (unsigned long) error);
      return( 1);
   }

   WaitForSingleObject( pi.hProcess, INFINITE);
   CloseHandle( pi.hThread);
   CloseHandle( pi.hProcess);
   return( 0);
}


int   main( int argc, char **argv)
{
   char      *env[ 3];
   wchar_t   *block;
   size_t    capacity;
   char      *value;
   int       i;

   if( argc > 1)
   {
      value = getenv( "MULLE_SPAWN_TEST");
      printf( "child: MULLE_SPAWN_TEST=%s\n", value ? value : "<unset>");
      value = getenv( "MULLE_SPAWN_MARKER");
      printf( "child: MULLE_SPAWN_MARKER=%s\n", value ? value : "<unset>");
      value = getenv( "PATH");
      printf( "child: PATH is %s\n", value ? "set" : "<unset>");
      value = getenv( "WINEPATH");
      printf( "child: WINEPATH is %s\n", value ? "set" : "<unset>");
      return( 0);
   }

   /*
    * The marker is what the test in test/10-api asserts about: a parent that
    * sets it, and a child that must not be able to see it.
    */
   SetEnvironmentVariableA( "MULLE_SPAWN_MARKER", "inherited");

   env[ 0] = "MULLE_SPAWN_TEST=replaced";
   env[ 1] = NULL;

   block = build_block( env, &capacity);
   if( ! block)
   {
      printf( "the block could not be built\n");
      return( 1);
   }

   printf( "capacity (in wchar_t): %lu\n", (unsigned long) capacity);
   printf( "block contents:         ");
   for( i = 0; i < (int) capacity; i++)
   {
      printf( "%04x ", (unsigned) block[ i]);
      if( i && ! block[ i] && ! block[ i + 1])
         break;
   }
   printf( "\n" );

   printf( "-- one entry, as mulle-spawn builds it --\n" );
   run_child( argv[ 0], (void *) block, 0);

   /*
    * The same block, announced as the UTF-16 block it is. This is what
    * mulle_spawn_open_with_options does now, and it is the row that decides
    * whether the earlier measurement was about the block or about the flag.
    */
   printf( "-- the same block, with CREATE_UNICODE_ENVIRONMENT --\n" );
   run_child( argv[ 0], (void *) block, CREATE_UNICODE_ENVIRONMENT);

   printf( "-- NULL, for comparison: the parent's environment --\n" );
   run_child( argv[ 0], NULL, 0);

   /*
    * The decisive one. The parent's own block is by definition what this
    * platform considers a valid environment block -- if handing it straight
    * back also produces a child that sees nothing, then nothing about the
    * format above can be the problem.
    */
   printf( "-- GetEnvironmentStringsW(), the parent's block, handed back --\n" );
   {
      wchar_t   *parent;

      parent = GetEnvironmentStringsW();
      printf( "     it starts %04x %04x %04x (%lu wchars)\n",
              (unsigned) parent[ 0], (unsigned) parent[ 1],
              (unsigned) parent[ 2], (unsigned long) wcslen( parent));
      run_child( argv[ 0], (void *) parent, CREATE_UNICODE_ENVIRONMENT);
      FreeEnvironmentStringsW( parent);
   }

   return( 0);
}
