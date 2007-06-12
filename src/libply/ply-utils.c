/* ply-utils.c -  random useful functions and macros
 *
 * Copyright (C) 2007 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include <config.h>

#include "ply-utils.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include <dlfcn.h>

#ifndef PLY_OPEN_FILE_DESCRIPTORS_DIR
#define PLY_OPEN_FILE_DESCRIPTORS_DIR "/proc/self/fd"
#endif

#ifndef PLY_ERRNO_STACK_SIZE
#define PLY_ERRNO_STACK_SIZE 256
#endif

#ifndef PLY_SOCKET_CONNECTION_BACKLOG
#define PLY_SOCKET_CONNECTION_BACKLOG 32
#endif

#ifndef PLY_SUPER_SECRET_LAZY_UNMOUNT_FLAG
#define PLY_SUPER_SECRET_LAZY_UNMOUNT_FLAG 2
#endif

static int errno_stack[PLY_ERRNO_STACK_SIZE];
static int errno_stack_position = 0;

bool 
ply_open_unidirectional_pipe (int *sender_fd,
                              int *receiver_fd)
{
  int pipe_fds[2];

  assert (sender_fd != NULL);
  assert (receiver_fd != NULL);

  if (pipe (pipe_fds) < 0)
    return false;

  if (fcntl (pipe_fds[0], F_SETFD, O_NONBLOCK | FD_CLOEXEC) < 0)
    {
      ply_save_errno ();
      close (pipe_fds[0]);
      close (pipe_fds[1]);
      ply_restore_errno ();
      return false;
    }

  if (fcntl (pipe_fds[1], F_SETFD, O_NONBLOCK | FD_CLOEXEC) < 0)
    {
      ply_save_errno ();
      close (pipe_fds[0]);
      close (pipe_fds[1]);
      ply_restore_errno ();
      return false;
    }

  *sender_fd = pipe_fds[1];
  *receiver_fd = pipe_fds[0];

  return true;
}

static int
ply_open_unix_socket (const char *path)
{
  int fd;

  assert (path != NULL);

  fd = socket (PF_UNIX, SOCK_STREAM, 0);

  if (fd < 0)
    return -1;

  if (fcntl (fd, F_SETFD, O_NONBLOCK | FD_CLOEXEC) < 0)
    {
      ply_save_errno ();
      close (fd);
      ply_restore_errno ();

      return -1;
    }

  return fd;
}

static struct sockaddr *
create_unix_address_from_path (const char *path,
                               bool        is_abstract,
                               size_t     *address_size)
{
  struct sockaddr_un *address; 

  assert (path != NULL);
  assert (strlen (path) < sizeof (address->sun_family));

  address = calloc (1, sizeof (struct sockaddr_un));
  address->sun_family = AF_UNIX;

  /* a socket is marked as abstract if its path has the
   * NUL byte at the beginning of the buffer instead of
   * the end
   * 
   * Note, we depend on the memory being zeroed by the calloc
   * call above.
   */
  if (!is_abstract)
    strncpy (address->sun_path, path, sizeof (address->sun_path) - 1);
  else
    strncpy (address->sun_path + 1, path, sizeof (address->sun_path) - 1);

  if (address_size != NULL)
    *address_size = sizeof (struct sockaddr_un);

  return (struct sockaddr *) address;
}

int
ply_connect_to_unix_socket (const char *path,
                            bool        is_abstract)
{
  struct sockaddr *address; 
  size_t address_size;
  int fd;
  
  fd = ply_open_unix_socket (path);

  if (fd < 0)
    return -1;

  address = create_unix_address_from_path (path, is_abstract, &address_size);

  if (connect (fd, address, address_size) < 0)
    {
      ply_save_errno ();
      free (address);
      close (fd);
      ply_restore_errno ();

      return -1;
    }
  free (address);

  return fd;
}

int
ply_listen_to_unix_socket (const char *path,
                           bool        is_abstract)
{
  struct sockaddr *address; 
  size_t address_size;
  int fd;
  
  fd = ply_open_unix_socket (path);

  if (fd < 0)
    return -1;

  address = create_unix_address_from_path (path, is_abstract, &address_size);

  if (bind (fd, address, address_size) < 0)
    {
      ply_save_errno ();
      free (address);
      close (fd);
      ply_restore_errno ();

      return -1;
    }

  free (address);

  if (listen (fd, PLY_SOCKET_CONNECTION_BACKLOG) < 0)
    {
      ply_save_errno ();
      close (fd);
      ply_restore_errno ();
      return -1;
    }

  if (!is_abstract)
    {
      if (fchmod (fd, 0600) < 0)
        {
          ply_save_errno ();
          close (fd);
          ply_restore_errno ();
          return -1;
        }
    }

  return fd;
}

int
ply_create_unix_socket (const char *path)
{
  struct sockaddr_un address; 
  int fd;

  assert (path != NULL);

  fd = socket (PF_UNIX, SOCK_STREAM, 0);

  if (fd < 0)
    return -1;

  if (fcntl (fd, F_SETFD, O_NONBLOCK | FD_CLOEXEC) < 0)
    {
      ply_save_errno ();
      close (fd);
      ply_restore_errno ();

      return -1;
    }

  memset (&address, 0, sizeof (address));

  address.sun_family = AF_UNIX;
  memcpy (address.sun_path, path, strlen (path));

  if (connect (fd, (struct sockaddr *) &address,
               sizeof (struct sockaddr_un)) < 0)
    {
      ply_save_errno ();
      close (fd);
      ply_restore_errno ();

      return -1;
    }

  return fd;
}

bool 
ply_write (int         fd,
           const void *buffer,
           size_t      number_of_bytes)
{
  size_t bytes_left_to_write;
  size_t total_bytes_written = 0;

  assert (fd >= 0);

  bytes_left_to_write = number_of_bytes;

  do
    {
      ssize_t bytes_written = 0;

      bytes_written = write (fd,
                             ((uint8_t *) buffer) + total_bytes_written,
                             bytes_left_to_write);

      if (bytes_written > 0)
        {
          total_bytes_written += bytes_written;
          bytes_left_to_write -= bytes_written;
        }
      else if ((errno != EINTR))
        break;
    }
  while (bytes_left_to_write > 0);

  return bytes_left_to_write == 0;
}

static ssize_t
ply_read_some_bytes (int     fd,
                     void   *buffer,
                     size_t  max_bytes)
{
  size_t bytes_left_to_read;
  size_t total_bytes_read = 0;

  assert (fd >= 0);

  bytes_left_to_read = max_bytes;

  do
    {
      ssize_t bytes_read = 0;

      bytes_read = read (fd,
                         ((uint8_t *) buffer) + total_bytes_read,
                         bytes_left_to_read);

      if (bytes_read > 0)
        {
          total_bytes_read += bytes_read;
          bytes_left_to_read -= bytes_read;
        }
      else if ((errno != EINTR))
        break;
    }
  while (bytes_left_to_read > 0);

  if ((bytes_left_to_read > 0) && (errno != EAGAIN))
    total_bytes_read = -1;

  return total_bytes_read;
}

bool 
ply_read (int     fd,
          void   *buffer,
          size_t  number_of_bytes)
{
  size_t total_bytes_read;
  bool read_was_successful;

  assert (fd >= 0);
  assert (buffer != NULL);
  assert (number_of_bytes != 0);

  total_bytes_read = ply_read_some_bytes (fd, buffer, number_of_bytes);

  read_was_successful = total_bytes_read == number_of_bytes;

  return read_was_successful;
}

bool 
ply_fd_has_data (int fd)
{
  struct pollfd poll_data;
  int result;

  poll_data.fd = fd;
  poll_data.events = POLLIN | POLLPRI;
  poll_data.revents = 0;
  result = poll (&poll_data, 1, 10);

  return result == 1 
         && ((poll_data.revents & POLLIN) 
         || (poll_data.revents & POLLPRI));
}

bool 
ply_fd_can_take_data (int fd)
{
  struct pollfd poll_data;
  int result;

  poll_data.fd = fd;
  poll_data.events = POLLOUT;
  poll_data.revents = 0;
  result = poll (&poll_data, 1, 10);

  return result == 1;
}

bool
ply_fd_may_block (int fd)
{
  int flags;

  assert (fd >= 0);

  flags = fcntl (fd, F_GETFL);

  return (flags & O_NONBLOCK) != 0;
}

char **
ply_copy_string_array (const char * const *array)
{
  char **copy;
  int i;

  for (i = 0; array[i] != NULL; i++);

  copy = calloc (i + 1, sizeof (char *));

  for (i = 0; array[i] != NULL; i++)
    copy[i] = strdup (array[i]);

  return copy;
}

void 
ply_free_string_array (char **array)
{
  int i;

  if (array == NULL)
    return;

  for (i = 0; array[i] != NULL; i++)
    {
      free (array[i]);
      array[i] = NULL;
    }

  free (array);
}

static int
ply_get_max_open_fds (void)
{
  struct rlimit open_fd_limit;

  if (getrlimit (RLIMIT_NOFILE, &open_fd_limit) < 0) 
    return -1;

  if (open_fd_limit.rlim_cur == RLIM_INFINITY) 
    return -1;

  return (int) open_fd_limit.rlim_cur;
}

static bool
ply_close_open_fds (void)
{
  DIR *dir;
  struct dirent *entry;
  int fd, opendir_fd;

  opendir_fd = -1;
  dir = opendir (PLY_OPEN_FILE_DESCRIPTORS_DIR);

  if (dir == NULL)
    return false;

  while ((entry = readdir (dir)) != NULL) 
    {
      long filename_as_number;
      char *byte_after_number;

      errno = 0;
      if (entry->d_name[0] == '.')
        continue;

      fd = -1;
      filename_as_number = strtol (entry->d_name, &byte_after_number, 10);

      if (byte_after_number != NULL)
        continue;

      if ((*byte_after_number != '\0') ||
          (filename_as_number < 0) ||
          (filename_as_number > INT_MAX)) 
        return false;

      fd = (int) filename_as_number;

      if (fd != opendir_fd)
        close (fd);
    }

  assert (entry == NULL);
  closedir (dir);

  return true;
}

void 
ply_close_all_fds (void)
{
  int max_open_fds, fd;

  max_open_fds = ply_get_max_open_fds ();

  /* if there isn't a reported maximum for some
   * reason, then open up /proc/self/fd and close
   * the ones we can find.  If that doesn't work
   * out, then just bite the bullet and close the
   * entire integer range
   */
  if (max_open_fds < 0)
    {
      if (ply_close_open_fds ())
        return;

      max_open_fds = INT_MAX;
    }

  else for (fd = 0; fd < max_open_fds; fd++) 
    close (fd);
}

double 
ply_get_timestamp (void)
{
  const double microseconds_per_second = 1000000.0;
  double timestamp;
  struct timeval now = { 0L, /* zero-filled */ };

  gettimeofday (&now, NULL);
  timestamp = ((microseconds_per_second * now.tv_sec) + now.tv_usec) /
               microseconds_per_second;

  return timestamp;
}

void 
ply_save_errno (void)
{
  assert (errno_stack_position < PLY_ERRNO_STACK_SIZE);
  errno_stack[errno_stack_position] = errno;
  errno_stack_position++;
}

void
ply_restore_errno (void)
{
  assert (errno_stack_position > 0);
  errno_stack_position--;
  errno = errno_stack[errno_stack_position];
}

bool 
ply_directory_exists (const char *dir)
{
  struct stat file_info;
  
  if (stat (dir, &file_info) < 0)
    return false;

  return S_ISDIR (file_info.st_mode);
}

bool
ply_file_exists (const char *file)
{
  struct stat file_info;
  
  if (stat (file, &file_info) < 0)
    return false;

  return S_ISREG (file_info.st_mode);
}

ply_module_handle_t *
ply_open_module (const char *module_path)
{
  ply_module_handle_t *handle;

  assert (module_path != NULL);

  handle = (ply_module_handle_t *) dlopen (module_path, RTLD_NOW | RTLD_LOCAL);

  if (handle == NULL)
    {
      dlerror ();
      if (errno == 0)
        errno = ELIBACC;
    }

  return handle;
}

ply_module_function_t
ply_module_look_up_function (ply_module_handle_t *handle,
                             const char          *function_name)
{
  ply_module_function_t function;

  assert (handle != NULL);
  assert (function_name != NULL);

  dlerror ();
  function = (ply_module_function_t) dlsym (handle, function_name);

  if (dlerror () != NULL)
    {
      if (errno == 0)
        errno = ELIBACC;

      return NULL;
    }

  return function;
}

void
ply_close_module (ply_module_handle_t *handle)
{
  dlclose (handle);
}

bool
ply_create_directory (const char *directory)
{
  assert (directory != NULL);
  assert (directory[0] != '\0');

  if (ply_directory_exists (directory))
    return true;

  if (ply_file_exists (directory))
    {
      errno = EEXIST;
      return false;
    }

  if (mkdir (directory, 0755) < 0)
    {
      char *parent_directory;
      char *last_path_component;
      bool parent_is_created;

      parent_is_created = false;
      if (errno == ENOENT)
        {
          parent_directory = strdup (directory);
          last_path_component = strrchr (parent_directory, '/');
          *last_path_component = '\0';
          parent_is_created = ply_create_directory (parent_directory);

          ply_save_errno ();
          free (parent_directory);
          ply_restore_errno ();
        }

      return parent_is_created;
    }


  return true;
}

bool 
ply_create_scratch_directory (const char *directory)
{
  int dir_fd;
  assert (directory != NULL);
  assert (directory[0] != NULL);
  
  if (!ply_create_directory (directory))
    return false;

  if (mount ("none", directory, "ramfs", 0, NULL) < 0)
    return false;

  dir_fd = open (directory, O_RDONLY);

  if (dir_fd < 0)
    {
      ply_save_errno ();
      umount (directory);
      ply_restore_errno ();
      return false;
    }

  umount2 (directory, PLY_SUPER_SECRET_LAZY_UNMOUNT_FLAG);

  /* contents of directory will remain visible to process
   * (and children) until the file descriptor is closed.
   *
   * We're leaking the file descriptor below, so it will
   * remain around until the process exits
   */

  return true;
}

/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */