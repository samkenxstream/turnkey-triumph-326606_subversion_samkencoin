/* fs-test.c --- tests for the filesystem
 *
 * ====================================================================
 * Copyright (c) 2000-2001 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 * ====================================================================
 */

#include <stdlib.h>
#include <string.h>
#include <apr_pools.h>
#include "svn_error.h"
#include "svn_fs.h"
#include "svn_path.h"
#include "svn_delta.h"

#include "../../libsvn_fs/fs.h"
#include "../../libsvn_fs/dag.h"
#include "../../libsvn_fs/rev-table.h"
#include "../../libsvn_fs/nodes-table.h"
#include "../../libsvn_fs/trail.h"

#include "dir-delta-editor.h"

/* A global pool, initialized by `main' for tests to use.  */
apr_pool_t *pool;


/*-------------------------------------------------------------------*/

/** Helper routines. **/



static void
berkeley_error_handler (const char *errpfx,
                        char *msg)
{
  fprintf (stderr, "%s%s\n", errpfx ? errpfx : "", msg);
}


/* Set *FS_P to a fresh, unopened FS object, with the right warning
   handling function set.  */
static svn_error_t *
fs_new (svn_fs_t **fs_p)
{
  *fs_p = svn_fs_new (pool);
  if (! *fs_p)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "Couldn't alloc a new fs object.");

  /* Provide a warning function that just dumps the message to stderr.  */
  svn_fs_set_warning_func (*fs_p, svn_handle_warning, 0);

  return SVN_NO_ERROR;
}


/* Create a berkeley db repository in a subdir NAME, and return a new
   FS object which points to it.  */
static svn_error_t *
create_fs_and_repos (svn_fs_t **fs_p, const char *name)
{
  apr_finfo_t finfo;

  /* If there's already a repository named NAME, delete it.  Doing
     things this way means that repositories stick around after a
     failure for postmortem analysis, but also that tests can be
     re-run without cleaning out the repositories created by prior
     runs.  */
  if (apr_stat (&finfo, name, APR_FINFO_TYPE, pool) == APR_SUCCESS)
    {
      if (finfo.filetype == APR_DIR)
        SVN_ERR (svn_fs_delete_berkeley (name, pool));
      else
        return svn_error_createf (SVN_ERR_TEST_FAILED, 0, NULL, pool,
                                  "there is already a file named `%s'", name);
    }

  SVN_ERR (fs_new (fs_p));
  SVN_ERR (svn_fs_create_berkeley (*fs_p, name));
  
  /* Provide a handler for Berkeley DB error messages.  */
  SVN_ERR (svn_fs_set_berkeley_errcall (*fs_p, berkeley_error_handler));

  return SVN_NO_ERROR;
}


/* Read all data from a generic read STREAM, and return it in STRING.
   Allocate the svn_string_t in APRPOOL.  (All data in STRING will be
   dup'ed from STREAM using APRPOOL too.) */
static svn_error_t *
stream_to_string (svn_string_t **string,
                  svn_stream_t *stream)
{
  char buf[50];
  apr_size_t len;
  svn_string_t *str = svn_string_create ("", pool);

  do 
    {
      /* "please read 40 bytes into buf" */
      len = 40;
      SVN_ERR (svn_stream_read (stream, buf, &len));
      
      /* Now copy however many bytes were *actually* read into str. */
      svn_string_appendbytes (str, buf, len);
      
    } while (len);  /* Continue until we're told that no bytes were
                       read. */

  *string = str;
  return SVN_NO_ERROR;
}

static svn_error_t *
set_file_contents (svn_fs_root_t *root,
                   const char *path,
                   const char *contents)
{
  svn_txdelta_window_handler_t consumer_func;
  void *consumer_baton;
  svn_string_t *wstring = svn_string_create (contents, pool);

  SVN_ERR (svn_fs_apply_textdelta (&consumer_func, &consumer_baton,
                                   root, path, pool));
  SVN_ERR (svn_txdelta_send_string (wstring, consumer_func,
                                    consumer_baton, pool));

  return SVN_NO_ERROR;
}


static svn_error_t *
get_file_contents (svn_fs_root_t *root,
                   const char *path,
                   svn_string_t **str)
{
  svn_stream_t *stream;

  SVN_ERR (svn_fs_file_contents (&stream, root, path, pool));  
  SVN_ERR (stream_to_string (str, stream));

  return SVN_NO_ERROR;
}



/* The Helper Functions to End All Helper Functions */

/* Structure used for testing integrity of the filesystem's revision
   using validate_tree().  cmpilato todo: does this need namespace
   protection? */
typedef struct tree_test_entry_t
{
  const char *path;     /* full path of this node */
  const char *contents; /* text contents (NULL for directories) */
}
tree_test_entry_t;
  

typedef struct tree_test_t
{
  tree_test_entry_t *entries;
  int num_entries;
}
tree_test_t;


/* Read all the entries in directory PATH under transaction or
   revision root ROOT, copying their full paths into the TREE_ENTRIES
   hash, and recursing when those entries are directories */
static svn_error_t *
get_dir_entries (apr_hash_t *tree_entries,
                 svn_fs_root_t *root,
                 svn_string_t *path)
{
  apr_hash_t *entries;
  apr_hash_index_t *hi;

  SVN_ERR (svn_fs_dir_entries (&entries, root, path->data, pool));
  
  /* Copy this list to the master list with the path prepended to the
     names */
  for (hi = apr_hash_first (entries); hi; hi = apr_hash_next (hi))
    {
      const void *key;
      apr_size_t keylen;
      void *val;
      svn_fs_dirent_t *dirent;
      svn_string_t *full_path;
      int is_dir;
 
      apr_hash_this (hi, &key, &keylen, &val);
      dirent = val;

      /* Calculate the full path of this entry (by appending the name
         to the path thus far) */
      full_path = svn_string_dup (path, pool);
      svn_path_add_component (full_path, 
                              svn_string_create (dirent->name, pool),
                              svn_path_repos_style); 

      /* Now, copy this dirent to the master hash, but this time, use
         the full path for the key */
      apr_hash_set (tree_entries, full_path->data, 
                    APR_HASH_KEY_STRING, dirent);

      /* If this entry is a directory, recurse into the tree. */
      SVN_ERR (svn_fs_is_dir (&is_dir, root, full_path->data, pool));
      if (is_dir)
        SVN_ERR (get_dir_entries (tree_entries, root, full_path));
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
validate_tree_entry (svn_fs_root_t *root,
                     tree_test_entry_t *entry)
{
  svn_stream_t *rstream;
  svn_string_t *rstring;
  int is_dir;

  /* Verify that this is the expected type of node */
  SVN_ERR (svn_fs_is_dir (&is_dir, root, entry->path, pool));
  if ((!is_dir && !entry->contents) || (is_dir && entry->contents))
    return svn_error_createf
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "node `%s' in tree was of unexpected node type", 
       entry->path);

  /* Verify that the contents are as expected (files only) */
  if (! is_dir)
    {
      SVN_ERR (svn_fs_file_contents (&rstream, root, entry->path, pool));  
      SVN_ERR (stream_to_string (&rstring, rstream));
      if (! svn_string_compare (rstring, 
                                svn_string_create (entry->contents, pool)))
        return svn_error_createf 
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "node `%s' in tree had unexpected contents",
           entry->path);
    }

  return SVN_NO_ERROR;
}
                     


/* Given a transaction or revision root (ROOT), check to see if the
   tree that grows from that root has all the path entries, and only
   those entries, passed in the array ENTRIES (which is an array of
   NUM_ENTRIES tree_test_entry_t's) */
static svn_error_t *
validate_tree (svn_fs_root_t *root,
               tree_test_entry_t *entries,
               int num_entries)
{
  apr_hash_t *tree_entries;
  int i;
  svn_string_t *root_dir = svn_string_create ("", pool);
  
  /* Create our master hash for storing the entries */
  tree_entries = apr_hash_make (pool);
  
  /* Begin the recursive directory entry dig */
  SVN_ERR (get_dir_entries (tree_entries, root, root_dir));

  if (num_entries < apr_hash_count (tree_entries))
    return svn_error_create
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "unexpected number of items in tree (too many)");
  if (num_entries > apr_hash_count (tree_entries))
    return svn_error_create
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "unexpected number of items in tree (too few)");

  if ((num_entries > 0) && (! entries))
    return svn_error_create
      (SVN_ERR_TEST_FAILED, 0, NULL, pool,
       "validation requested against non-existant control data");

  for (i = 0; i < num_entries; i++)
    {
      void *val;
      
      /* Verify that the entry exists in our full list of entries. */
      val = apr_hash_get (tree_entries, entries[i].path, APR_HASH_KEY_STRING);
      if (! val)
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "failed to find expected node `%s' in tree", 
           entries[i].path);
      SVN_ERR (validate_tree_entry (root, &entries[i]));
    }
  return SVN_NO_ERROR;
}


typedef struct txn_script_command_t
{
  /* Command to perform:

     '+' - add
     '-' - delete
     '>' - replace contents
  */
  int cmd;
  const char *path; /* path to resource in the filesystem */
  const char *contents; /* file contents, 0 for directories */
} 
txn_script_command_t;


static svn_error_t *
txn_script_exec (svn_fs_root_t *txn_root,
                 txn_script_command_t *script,
                 int num_edits)
{
  int i;

  /* Run through the list of edits, making the appropriate edit on
     that entry in the TXN_ROOT. */ 
  for (i = 0; i < num_edits; i++)
    {
      const char *path = script[i].path;
      const char *contents = script[i].contents;
      int cmd = script[i].cmd;
      int is_dir = (contents == 0);
 
      switch (cmd)
        {
        case '+':
          if (is_dir)
            {
              SVN_ERR (svn_fs_make_dir (txn_root, path, pool));
            }
          else
            {
              SVN_ERR (svn_fs_make_file (txn_root, path, pool));
              SVN_ERR (set_file_contents (txn_root, path, contents));
            }
          break;

        case '-':
          SVN_ERR (svn_fs_delete_tree (txn_root, path, pool));
          break;

        case '>':
          if (! is_dir)
            {
              SVN_ERR (set_file_contents (txn_root, path, contents));
            }
          break;

        default:
          break;
        }
    }

  return SVN_NO_ERROR;
}



/*-----------------------------------------------------------------*/

/** The actual fs-tests called by `make check` **/

/* Create a filesystem.  */
static svn_error_t *
create_berkeley_filesystem (const char **msg)
{
  svn_fs_t *fs;

  *msg = "svn_fs_create_berkeley";

  /* Create and close a repository. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-1")); /* helper */
  SVN_ERR (svn_fs_close_fs (fs));
  
  return SVN_NO_ERROR;
}


/* Open an existing filesystem.  */
static svn_error_t *
open_berkeley_filesystem (const char **msg)
{
  svn_fs_t *fs, *fs2;

  *msg = "open an existing Berkeley DB filesystem";

  /* Create and close a repository (using fs). */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-2")); /* helper */
  SVN_ERR (svn_fs_close_fs (fs));

  /* Create a different fs object, and use it to re-open the
     repository again.  */
  SVN_ERR (fs_new (&fs2));
  SVN_ERR (svn_fs_open_berkeley (fs2, "test-repo-2"));

  /* Provide a handler for Berkeley DB error messages.  */
  SVN_ERR (svn_fs_set_berkeley_errcall (fs2, berkeley_error_handler));

  SVN_ERR (svn_fs_close_fs (fs2));

  return SVN_NO_ERROR;
}


/* Begin a txn, check its name, then close it */
static svn_error_t *
trivial_transaction (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  const char *txn_name;

  *msg = "begin a txn, check its name, then close it";

  SVN_ERR (create_fs_and_repos (&fs, "test-repo-4")); /* helper */

  /* Begin a new transaction that is based on revision 0.  */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
      
  /* Test that the txn name is non-null. */
  SVN_ERR (svn_fs_txn_name (&txn_name, txn, pool));
  
  if (! txn_name)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "Got a NULL txn name.");

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}



/* Open an existing transaction by name. */
static svn_error_t *
reopen_trivial_transaction (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  const char *txn_name;

  *msg = "open an existing transaction by name";

  SVN_ERR (create_fs_and_repos (&fs, "test-repo-5")); /* helper */

  /* Begin a new transaction that is based on revision 0.  */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_name (&txn_name, txn, pool));

  /* Close the transaction. */
  SVN_ERR (svn_fs_close_txn (txn));

  /* Reopen the transaction by name */
  SVN_ERR (svn_fs_open_txn (&txn, fs, txn_name, pool));

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}



/* Create a file! */
static svn_error_t *
create_file_transaction (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;

  *msg = "begin a txn, get the txn root, and add a file!";

  SVN_ERR (create_fs_and_repos (&fs, "test-repo-6")); /* helper */

  /* Begin a new transaction that is based on revision 0.  */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));

  /* Get the txn root */
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  
  /* Create a new file in the root directory. */
  SVN_ERR (svn_fs_make_file (txn_root, "beer.txt", pool));

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


static svn_error_t *
check_no_fs_error (svn_error_t *err)
{
  if (err && (err->apr_err != SVN_ERR_FS_NOT_OPEN))
    return svn_error_create
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "checking not opened filesystem got wrong error");
  else if (! err)
    return svn_error_create
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "checking not opened filesytem failed to get error");
  else
    return SVN_NO_ERROR;
}


/* Call functions with not yet opened filesystem and see it returns
   correct error.  */
static svn_error_t *
call_functions_with_unopened_fs (const char **msg)
{
  svn_error_t *err;
  svn_fs_t *fs = svn_fs_new (pool);

  *msg = "Call functions with unopened filesystem and check errors";

  /* This is the exception --- it is perfectly okay to call
     svn_fs_close_fs on an unopened filesystem.  */
  SVN_ERR (svn_fs_close_fs (fs));

  fs = svn_fs_new (pool);
  err = svn_fs_set_berkeley_errcall (fs, berkeley_error_handler);
  SVN_ERR (check_no_fs_error (err));

  {
    svn_fs_txn_t *ignored;
    err = svn_fs_begin_txn (&ignored, fs, 0, pool);
    SVN_ERR (check_no_fs_error (err));
    err = svn_fs_open_txn (&ignored, fs, "0", pool);
    SVN_ERR (check_no_fs_error (err));
  }

  {
    char **ignored;
    err = svn_fs_list_transactions (&ignored, fs, pool);
    SVN_ERR (check_no_fs_error (err));
  }

  {
    svn_fs_root_t *ignored;
    err = svn_fs_revision_root (&ignored, fs, 0, pool);
    SVN_ERR (check_no_fs_error (err));
  }

  {
    svn_revnum_t ignored;
    err = svn_fs_youngest_rev (&ignored, fs, pool);
    SVN_ERR (check_no_fs_error (err));
  }

  {
    svn_string_t *ignored, *unused;
    err = svn_fs_revision_prop (&ignored, fs, 0, unused, pool);
    SVN_ERR (check_no_fs_error (err));
  }

  {
    apr_hash_t *ignored;
    err = svn_fs_revision_proplist (&ignored, fs, 0, pool);
    SVN_ERR (check_no_fs_error (err));
  }

  {
    svn_string_t *unused1, *unused2;
    err = svn_fs_change_rev_prop (fs, 0, unused1, unused2, pool);
    SVN_ERR (check_no_fs_error (err));
  }

  return SVN_NO_ERROR;
}


/* Make sure we get txn lists correctly. */
static svn_error_t *
verify_txn_list (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn1, *txn2;
  const char *name1, *name2;
  char **txn_list;

  *msg = "create 2 txns, list them, and verify the list.";

  SVN_ERR (create_fs_and_repos (&fs, "test-repo-7")); /* helper */

  /* Begin a new transaction, get its name, close it.  */
  SVN_ERR (svn_fs_begin_txn (&txn1, fs, 0, pool));
  SVN_ERR (svn_fs_txn_name (&name1, txn1, pool));
  SVN_ERR (svn_fs_close_txn (txn1));

  /* Begin *another* transaction, get its name, close it.  */
  SVN_ERR (svn_fs_begin_txn (&txn2, fs, 0, pool));
  SVN_ERR (svn_fs_txn_name (&name2, txn2, pool));
  SVN_ERR (svn_fs_close_txn (txn2));

  /* Get the list of active transactions from the fs. */
  SVN_ERR (svn_fs_list_transactions (&txn_list, fs, pool));

  /* Check the list. It should have *exactly* two entries. */
  if ((txn_list[0] == NULL)
      || (txn_list[1] == NULL)
      || (txn_list[2] != NULL))
    goto all_bad;
  
  /* We should be able to find our 2 txn names in the list, in some
     order. */
  if ((! strcmp (txn_list[0], name1))
      && (! strcmp (txn_list[1], name2)))
    goto all_good;
  
  else if ((! strcmp (txn_list[1], name1))
           && (! strcmp (txn_list[0], name2)))
    goto all_good;
  
 all_bad:

  return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                           "Got a bogus txn list.");
 all_good:
  
  /* Close the fs. */
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}



/* Test writing & reading a file's contents. */
static svn_error_t *
write_and_read_file (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  svn_stream_t *rstream;
  svn_string_t *rstring;
  svn_string_t *wstring = svn_string_create ("Wicki wild, wicki wicki wild.",
                                             pool);

  *msg = "write and read a file's contents";

  SVN_ERR (create_fs_and_repos (&fs, "test-repo-8")); /* helper */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  
  /* Add an empty file. */
  SVN_ERR (svn_fs_make_file (txn_root, "beer.txt", pool));

  /* And write some data into this file. */
  SVN_ERR (set_file_contents (txn_root, "beer.txt", wstring->data));
  
  /* Now let's read the data back from the file. */
  SVN_ERR (svn_fs_file_contents (&rstream, txn_root, "beer.txt", pool));  
  SVN_ERR (stream_to_string (&rstring, rstream));

  /* Compare what was read to what was written. */
  if (! svn_string_compare (rstring, wstring))
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "data read != data written.");    

  /* Clean up the repos. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}



/* Create a file, a directory, and a file in that directory! */
static svn_error_t *
create_mini_tree_transaction (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;

  *msg = "make a file, a subdir, and another file in that subdir!";

  SVN_ERR (create_fs_and_repos (&fs, "test-repo-9")); /* helper */

  /* Begin a new transaction that is based on revision 0.  */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));

  /* Get the txn root */
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  
  /* Create a new file in the root directory. */
  SVN_ERR (svn_fs_make_file (txn_root, "wine.txt", pool));

  /* Create a new directory in the root directory. */
  SVN_ERR (svn_fs_make_dir (txn_root, "keg", pool));

  /* Now, create a file in our new directory. */
  SVN_ERR (svn_fs_make_file (txn_root, "keg/beer.txt", pool));

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


/* Helper function to verify contents of Greek Tree.  */
static svn_error_t *
check_greek_tree_under_root (svn_fs_root_t *rev_root)
{
  svn_stream_t *rstream;
  svn_string_t *rstring;
  svn_string_t *content;
  int i;

  const char *file_contents[12][2] =
  {
    { "iota", "This is the file 'iota'.\n" },
    { "A/mu", "This is the file 'mu'.\n" },
    { "A/B/lambda", "This is the file 'lambda'.\n" },
    { "A/B/E/alpha", "This is the file 'alpha'.\n" },
    { "A/B/E/beta", "This is the file 'beta'.\n" },
    { "A/D/gamma", "This is the file 'gamma'.\n" },
    { "A/D/G/pi", "This is the file 'pi'.\n" },
    { "A/D/G/rho", "This is the file 'rho'.\n" },
    { "A/D/G/tau", "This is the file 'tau'.\n" },
    { "A/D/H/chi", "This is the file 'chi'.\n" },
    { "A/D/H/psi", "This is the file 'psi'.\n" },
    { "A/D/H/omega", "This is the file 'omega'.\n" }
  };

  /* Loop through the list of files, checking for matching content. */
  for (i = 0; i < 12; i++)
    {
      SVN_ERR (svn_fs_file_contents (&rstream, rev_root, 
                                     file_contents[i][0], pool));
      SVN_ERR (stream_to_string (&rstring, rstream));
      content = svn_string_create (file_contents[i][1], pool);
      if (! svn_string_compare (rstring, content))
        return svn_error_createf (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                                 "data read != data written in file `%s'.",
                                 file_contents[i][0]);
    }
  return SVN_NO_ERROR;
}


/* Helper for the various functions that operate on the Greek Tree:
   creates the Greek Tree under TXN_ROOT.  See ../greek-tree.txt.  */
static svn_error_t *
greek_tree_under_root (svn_fs_root_t *txn_root)
{
  SVN_ERR (svn_fs_make_file (txn_root, "iota", pool));
  SVN_ERR (set_file_contents (txn_root, "iota",
                              "This is the file 'iota'.\n"));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/mu", pool));
  SVN_ERR (set_file_contents (txn_root, "A/mu",
                              "This is the file 'mu'.\n"));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A/B", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/B/lambda", pool));
  SVN_ERR (set_file_contents (txn_root, "A/B/lambda",
                              "This is the file 'lambda'.\n"));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A/B/E", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/B/E/alpha", pool));
  SVN_ERR (set_file_contents (txn_root, "A/B/E/alpha",
                              "This is the file 'alpha'.\n"));
  SVN_ERR (svn_fs_make_file (txn_root, "A/B/E/beta", pool));
  SVN_ERR (set_file_contents (txn_root, "A/B/E/beta",
                              "This is the file 'beta'.\n"));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A/B/F", pool));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A/C", pool));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A/D", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/gamma", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/gamma",
                              "This is the file 'gamma'.\n"));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A/D/G", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/G/pi", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/G/pi",
                              "This is the file 'pi'.\n"));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/G/rho", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/G/rho",
                              "This is the file 'rho'.\n"));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/G/tau", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/G/tau",
                              "This is the file 'tau'.\n"));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A/D/H", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/H/chi", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/H/chi",
                              "This is the file 'chi'.\n"));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/H/psi", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/H/psi",
                              "This is the file 'psi'.\n"));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/H/omega", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/H/omega",
                              "This is the file 'omega'.\n"));
  return SVN_NO_ERROR;
}


/* Create a file, a directory, and a file in that directory! */
static svn_error_t *
create_greek_tree_transaction (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;

  *msg = "make The Official Subversion Test Tree";

  /* Prepare a txn to receive the greek tree. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-10"));
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create and verify the greek tree. */
  SVN_ERR (greek_tree_under_root (txn_root));

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


/* Verify that entry KEY is present in ENTRIES, and that its value is
   an svn_fs_dirent_t whose name and id are not null. */
static svn_error_t *
verify_entry (apr_hash_t *entries, const char *key)
{
  svn_fs_dirent_t *ent = apr_hash_get (entries, key, 
                                       APR_HASH_KEY_STRING);

  if (ent == NULL)
    return svn_error_createf
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "didn't find dir entry for \"%s\"", key);

  if ((ent->name == NULL) && (ent->id == NULL))
    return svn_error_createf
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "dir entry for \"%s\" has null name and null id", key);
  
  if (ent->name == NULL)
    return svn_error_createf
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "dir entry for \"%s\" has null name", key);
  
  if (ent->id == NULL)
    return svn_error_createf
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "dir entry for \"%s\" has null id", key);
  
  if (strcmp (ent->name, key) != 0)
     return svn_error_createf
     (SVN_ERR_FS_GENERAL, 0, NULL, pool,
      "dir entry for \"%s\" contains wrong name (\"%s\")", key, ent->name);
        
  return SVN_NO_ERROR;
}


static svn_error_t *
list_directory (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  apr_hash_t *entries;

  *msg = "fill a directory, then list it";

  SVN_ERR (create_fs_and_repos (&fs, "test-repo-list-dir"));
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  
  /* We create this tree
   *
   *         /q
   *         /A/x
   *         /A/y
   *         /A/z
   *         /B/m
   *         /B/n
   *         /B/o
   *
   * then list dir A.  It should have 3 files: "x", "y", and "z", no
   * more, no less.
   */

  /* Create the tree. */
  SVN_ERR (svn_fs_make_file (txn_root, "q", pool));
  SVN_ERR (svn_fs_make_dir  (txn_root, "A", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/x", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/y", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/z", pool));
  SVN_ERR (svn_fs_make_dir  (txn_root, "B", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "B/m", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "B/n", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "B/o", pool));

  /* Get A's entries. */
  SVN_ERR (svn_fs_dir_entries (&entries, txn_root, "A", pool));

  /* Make sure exactly the right set of entries is present. */
  if (apr_hash_count (entries) != 3)
    {
      return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                               "unexpected number of entries in dir");
    }
  else
    {
      SVN_ERR (verify_entry (entries, "x"));
      SVN_ERR (verify_entry (entries, "y"));
      SVN_ERR (verify_entry (entries, "z"));
    }

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


static svn_error_t *
revision_props (const char **msg)
{
  svn_fs_t *fs;
  apr_hash_t *proplist;
  svn_string_t *value;
  int i;

  const char *initial_props[4][2] = { 
    { "color", "red" },
    { "size", "XXL" },
    { "favorite saturday morning cartoon", "looney tunes" },
    { "auto", "Green 1997 Saturn SL1" }
    };

  const char *final_props[4][2] = { 
    { "color", "violet" },
    { "flower", "violet" },
    { "favorite saturday morning cartoon", "looney tunes" },
    { "auto", "Red 2000 Chevrolet Blazer" }
    };

  *msg = "set and get some revision properties";

  /* Open the fs */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-rev-props"));

  /* Set some properties on the revision. */
  for (i = 0; i < 4; i++)
    {
      SVN_ERR (svn_fs_change_rev_prop 
               (fs, 0, 
                svn_string_create (initial_props[i][0], pool),
                svn_string_create (initial_props[i][1], pool), 
                pool));
    }

  /* Change some of the above properties. */
  SVN_ERR (svn_fs_change_rev_prop 
           (fs, 0, 
            svn_string_create ("color", pool),
            svn_string_create ("violet", pool), 
            pool));
  SVN_ERR (svn_fs_change_rev_prop 
           (fs, 0, 
            svn_string_create ("auto", pool),
            svn_string_create ("Red 2000 Chevrolet Blazer", pool), 
            pool));

  /* Remove a property altogether */
  SVN_ERR (svn_fs_change_rev_prop 
           (fs, 0, 
            svn_string_create ("size", pool),
            NULL,
            pool));

  /* Copy a property's value into a new property. */
  SVN_ERR (svn_fs_revision_prop 
           (&value, 
            fs, 0, 
            svn_string_create ("color", pool),
            pool));
  SVN_ERR (svn_fs_change_rev_prop 
           (fs, 0, 
            svn_string_create ("flower", pool),
            value,
            pool));

  /* Obtain a list of all current properties, and make sure it matches
     the expected values. */
  SVN_ERR (svn_fs_revision_proplist (&proplist, fs, 0, pool));
  {
    svn_string_t *prop_value;

    if (apr_hash_count (proplist) != 4 )
      return svn_error_createf
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "unexpected number of revision properties were found");

    /* Loop through our list of expected revision property name/value
       pairs. */
    for (i = 0; i < 4; i++)
      {
        /* For each expected property: */

        /* Step 1.  Find it by name in the hash of all rev. props
           returned to us by svn_fs_revision_proplist.  If it can't be
           found, return an error. */
        prop_value = apr_hash_get (proplist, 
                                   final_props[i][0],
                                   APR_HASH_KEY_STRING);
        if (! prop_value)
          return svn_error_createf
            (SVN_ERR_FS_GENERAL, 0, NULL, pool,
             "unable to find expected revision property");

        /* Step 2.  Make sure the value associated with it is the same
           as what was expected, else return an error. */
        if (strcmp (prop_value->data, final_props[i][1]))
          return svn_error_createf
            (SVN_ERR_FS_GENERAL, 0, NULL, pool,
             "revision property had an unexpected value");
      }
  }
  
  /* Close the fs. */
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


static svn_error_t *
node_props (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  apr_hash_t *proplist;
  svn_string_t *value;
  int i;

  const char *initial_props[4][2] = { 
    { "Best Rock Artist", "Creed" },
    { "Best Rap Artist", "Eminem" },
    { "Best Country Artist", "(null)" },
    { "Best Sound Designer", "Pluessman" }
    };

  const char *final_props[4][2] = { 
    { "Best Rock Artist", "P.O.D." },
    { "Best Rap Artist", "Busta Rhymes" },
    { "Best Sound Designer", "Pluessman" },
    { "Biggest Cakewalk Fanatic", "Pluessman" }
    };

  *msg = "set and get some node properties";

  /* Open the fs and transaction */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-node-props"));
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Make a node to put some properties into */
  SVN_ERR (svn_fs_make_file (txn_root, "music.txt", pool));

  /* Set some properties on the nodes. */
  for (i = 0; i < 4; i++)
    {
      SVN_ERR (svn_fs_change_node_prop 
               (txn_root, "music.txt", 
                svn_string_create (initial_props[i][0], pool),
                svn_string_create (initial_props[i][1], pool), 
                pool));
    }

  /* Change some of the above properties. */
  SVN_ERR (svn_fs_change_node_prop 
           (txn_root, "music.txt", 
            svn_string_create ("Best Rock Artist", pool),
            svn_string_create ("P.O.D.", pool), 
            pool));
  SVN_ERR (svn_fs_change_node_prop 
           (txn_root, "music.txt", 
            svn_string_create ("Best Rap Artist", pool),
            svn_string_create ("Busta Rhymes", pool), 
            pool));

  /* Remove a property altogether */
  SVN_ERR (svn_fs_change_node_prop 
           (txn_root, "music.txt", 
            svn_string_create ("Best Country Artist", pool),
            NULL,
            pool));

  /* Copy a property's value into a new property. */
  SVN_ERR (svn_fs_node_prop 
           (&value, 
            txn_root, "music.txt", 
            svn_string_create ("Best Sound Designer", pool),
            pool));
  SVN_ERR (svn_fs_change_node_prop 
           (txn_root, "music.txt",
            svn_string_create ("Biggest Cakewalk Fanatic", pool),
            value,
            pool));

  /* Obtain a list of all current properties, and make sure it matches
     the expected values. */
  SVN_ERR (svn_fs_node_proplist (&proplist, txn_root, "music.txt", pool));
  {
    svn_string_t *prop_value;

    if (apr_hash_count (proplist) != 4 )
      return svn_error_createf
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "unexpected number of node properties were found");

    /* Loop through our list of expected node property name/value
       pairs. */
    for (i = 0; i < 4; i++)
      {
        /* For each expected property: */

        /* Step 1.  Find it by name in the hash of all node props
           returned to us by svn_fs_node_proplist.  If it can't be
           found, return an error. */
        prop_value = apr_hash_get (proplist, 
                                   final_props[i][0],
                                   APR_HASH_KEY_STRING);
        if (! prop_value)
          return svn_error_createf
            (SVN_ERR_FS_GENERAL, 0, NULL, pool,
             "unable to find expected node property");

        /* Step 2.  Make sure the value associated with it is the same
           as what was expected, else return an error. */
        if (strcmp (prop_value->data, final_props[i][1]))
          return svn_error_createf
            (SVN_ERR_FS_GENERAL, 0, NULL, pool,
             "node property had an unexpected value");
      }
  }
  
  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}



/* Set *PRESENT to true if entry NAME is present in directory PATH
   under ROOT, else set *PRESENT to false. */
static svn_error_t *
check_entry (svn_fs_root_t *root,
             const char *path,
             const char *name,
             svn_boolean_t *present)
{
  apr_hash_t *entries;
  svn_fs_dirent_t *ent;

  SVN_ERR (svn_fs_dir_entries (&entries, root, path, pool));
  ent = apr_hash_get (entries, name, APR_HASH_KEY_STRING);

  if (ent)
    *present = TRUE;
  else
    *present = FALSE;

  return SVN_NO_ERROR;
}


/* Return an error if entry NAME is absent in directory PATH under ROOT. */
static svn_error_t *
check_entry_present (svn_fs_root_t *root, const char *path, const char *name)
{
  svn_boolean_t present;
  SVN_ERR (check_entry (root, path, name, &present));

  if (! present)
    return svn_error_createf
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "entry \"%s\" absent when it should be present", name);

  return SVN_NO_ERROR;
}


/* Return an error if entry NAME is present in directory PATH under ROOT. */
static svn_error_t *
check_entry_absent (svn_fs_root_t *root, const char *path, const char *name)
{
  svn_boolean_t present;
  return check_entry (root, path, name, &present);

  if (present)
    return svn_error_createf
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "entry \"%s\" present when it should be absent", name);

  return SVN_NO_ERROR;
}


struct check_id_args
{
  svn_fs_t *fs;
  svn_fs_id_t *id;
  svn_boolean_t present;
};


static svn_error_t *
txn_body_check_id (void *baton, trail_t *trail)
{
  struct check_id_args *args = baton;
  skel_t *rep;
  svn_error_t *err;

  err = svn_fs__get_rep (&rep, args->fs, args->id, trail);

  if (err && (err->apr_err == SVN_ERR_FS_ID_NOT_FOUND))
    args->present = FALSE;
  else if (! err)
    args->present = TRUE;
  else
    {
      svn_string_t *id_str = svn_fs_unparse_id (args->id, pool);
      return svn_error_createf
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "error looking for node revision id \"%s\"", id_str->data);
    }

  return SVN_NO_ERROR;
}


/* Set *PRESENT to true if node revision ID is present in filesystem
   FS, else set *PRESENT to false. */
static svn_error_t *
check_id (svn_fs_t *fs, svn_fs_id_t *id, svn_boolean_t *present)
{
  struct check_id_args args;

  args.id = id;
  args.fs = fs;
  SVN_ERR (svn_fs__retry_txn (fs, txn_body_check_id, &args, pool));

  if (args.present)
    *present = TRUE;
  else
    *present = FALSE;

  return SVN_NO_ERROR;
}


/* Return error if node revision ID is not present in FS. */
static svn_error_t *
check_id_present (svn_fs_t *fs, svn_fs_id_t *id)
{
  svn_boolean_t present;
  SVN_ERR (check_id (fs, id, &present));

  if (! present)
    {
      svn_string_t *id_str = svn_fs_unparse_id (id, pool);
      return svn_error_createf
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "node revision id \"%s\" absent when should be present",
         id_str->data);
    }

  return SVN_NO_ERROR;
}


/* Return error if node revision ID is present in FS. */
static svn_error_t *
check_id_absent (svn_fs_t *fs, svn_fs_id_t *id)
{
  svn_boolean_t present;
  SVN_ERR (check_id (fs, id, &present));

  if (present)
    {
      svn_string_t *id_str = svn_fs_unparse_id (id, pool);
      return svn_error_createf
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "node revision id \"%s\" present when should be absent",
         id_str->data);
    }

  return SVN_NO_ERROR;
}


/* Test that aborting a Subversion transaction works.

   NOTE: This function tests internal filesystem interfaces, not just
   the public filesystem interface.  */
static svn_error_t *
abort_txn (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn1, *txn2;
  svn_fs_root_t *txn1_root, *txn2_root;
  const char *txn1_name, *txn2_name;

  *msg = "abort a transaction";

  /* Prepare two txns to receive the Greek tree. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-abort-txn"));
  SVN_ERR (svn_fs_begin_txn (&txn1, fs, 0, pool));
  SVN_ERR (svn_fs_begin_txn (&txn2, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn1_root, txn1, pool));
  SVN_ERR (svn_fs_txn_root (&txn2_root, txn2, pool));

  /* Save their names for later. */
  SVN_ERR (svn_fs_txn_name (&txn1_name, txn1, pool));
  SVN_ERR (svn_fs_txn_name (&txn2_name, txn2, pool));
  
  /* Create greek trees in them. */
  SVN_ERR (greek_tree_under_root (txn1_root));
  SVN_ERR (greek_tree_under_root (txn2_root));

  /* The test is to abort txn2, while leaving txn1.
   *
   * After we abort txn2, we make sure that a) all of its nodes
   * disappeared from the database, and b) none of txn1's nodes
   * disappeared.
   *
   * Finally, we create a third txn, and check that the name it got is
   * different from the names of txn1 and txn2.
   */

  {
    /* Yes, I really am this paranoid. */

    /* IDs for every file in the standard Greek Tree. */
    svn_fs_id_t
      *t1_root_id,    *t2_root_id,
      *t1_iota_id,    *t2_iota_id,
      *t1_A_id,       *t2_A_id,
      *t1_mu_id,      *t2_mu_id,
      *t1_B_id,       *t2_B_id,
      *t1_lambda_id,  *t2_lambda_id,
      *t1_E_id,       *t2_E_id,
      *t1_alpha_id,   *t2_alpha_id,
      *t1_beta_id,    *t2_beta_id,
      *t1_F_id,       *t2_F_id,
      *t1_C_id,       *t2_C_id,
      *t1_D_id,       *t2_D_id,
      *t1_gamma_id,   *t2_gamma_id,
      *t1_H_id,       *t2_H_id,
      *t1_chi_id,     *t2_chi_id,
      *t1_psi_id,     *t2_psi_id,
      *t1_omega_id,   *t2_omega_id,
      *t1_G_id,       *t2_G_id,
      *t1_pi_id,      *t2_pi_id,
      *t1_rho_id,     *t2_rho_id,
      *t1_tau_id,     *t2_tau_id;
    
    SVN_ERR (svn_fs_node_id (&t1_root_id, txn1_root, "", pool));
    SVN_ERR (svn_fs_node_id (&t2_root_id, txn2_root, "", pool));
    SVN_ERR (svn_fs_node_id (&t1_iota_id, txn1_root, "iota", pool));
    SVN_ERR (svn_fs_node_id (&t2_iota_id, txn2_root, "iota", pool));
    SVN_ERR (svn_fs_node_id (&t1_A_id, txn1_root, "/A", pool));
    SVN_ERR (svn_fs_node_id (&t2_A_id, txn2_root, "/A", pool));
    SVN_ERR (svn_fs_node_id (&t1_mu_id, txn1_root, "/A/mu", pool));
    SVN_ERR (svn_fs_node_id (&t2_mu_id, txn2_root, "/A/mu", pool));
    SVN_ERR (svn_fs_node_id (&t1_B_id, txn1_root, "/A/B", pool));
    SVN_ERR (svn_fs_node_id (&t2_B_id, txn2_root, "/A/B", pool));
    SVN_ERR (svn_fs_node_id (&t1_lambda_id, txn1_root, "/A/B/lambda", pool));
    SVN_ERR (svn_fs_node_id (&t2_lambda_id, txn2_root, "/A/B/lambda", pool));
    SVN_ERR (svn_fs_node_id (&t1_E_id, txn1_root, "/A/B/E", pool));
    SVN_ERR (svn_fs_node_id (&t2_E_id, txn2_root, "/A/B/E", pool));
    SVN_ERR (svn_fs_node_id (&t1_alpha_id, txn1_root, "/A/B/E/alpha", pool));
    SVN_ERR (svn_fs_node_id (&t2_alpha_id, txn2_root, "/A/B/E/alpha", pool));
    SVN_ERR (svn_fs_node_id (&t1_beta_id, txn1_root, "/A/B/E/beta", pool));
    SVN_ERR (svn_fs_node_id (&t2_beta_id, txn2_root, "/A/B/E/beta", pool));
    SVN_ERR (svn_fs_node_id (&t1_F_id, txn1_root, "/A/B/F", pool));
    SVN_ERR (svn_fs_node_id (&t2_F_id, txn2_root, "/A/B/F", pool));
    SVN_ERR (svn_fs_node_id (&t1_C_id, txn1_root, "/A/C", pool));
    SVN_ERR (svn_fs_node_id (&t2_C_id, txn2_root, "/A/C", pool));
    SVN_ERR (svn_fs_node_id (&t1_D_id, txn1_root, "/A/D", pool));
    SVN_ERR (svn_fs_node_id (&t2_D_id, txn2_root, "/A/D", pool));
    SVN_ERR (svn_fs_node_id (&t1_gamma_id, txn1_root, "/A/D/gamma", pool));
    SVN_ERR (svn_fs_node_id (&t2_gamma_id, txn2_root, "/A/D/gamma", pool));
    SVN_ERR (svn_fs_node_id (&t1_H_id, txn1_root, "/A/D/H", pool));
    SVN_ERR (svn_fs_node_id (&t2_H_id, txn2_root, "/A/D/H", pool));
    SVN_ERR (svn_fs_node_id (&t1_chi_id, txn1_root, "/A/D/H/chi", pool));
    SVN_ERR (svn_fs_node_id (&t2_chi_id, txn2_root, "/A/D/H/chi", pool));
    SVN_ERR (svn_fs_node_id (&t1_psi_id, txn1_root, "/A/D/H/psi", pool));
    SVN_ERR (svn_fs_node_id (&t2_psi_id, txn2_root, "/A/D/H/psi", pool));
    SVN_ERR (svn_fs_node_id (&t1_omega_id, txn1_root, "/A/D/H/omega", pool));
    SVN_ERR (svn_fs_node_id (&t2_omega_id, txn2_root, "/A/D/H/omega", pool));
    SVN_ERR (svn_fs_node_id (&t1_G_id, txn1_root, "/A/D/G", pool));
    SVN_ERR (svn_fs_node_id (&t2_G_id, txn2_root, "/A/D/G", pool));
    SVN_ERR (svn_fs_node_id (&t1_pi_id, txn1_root, "/A/D/G/pi", pool));
    SVN_ERR (svn_fs_node_id (&t2_pi_id, txn2_root, "/A/D/G/pi", pool));
    SVN_ERR (svn_fs_node_id (&t1_rho_id, txn1_root, "/A/D/G/rho", pool));
    SVN_ERR (svn_fs_node_id (&t2_rho_id, txn2_root, "/A/D/G/rho", pool));
    SVN_ERR (svn_fs_node_id (&t1_tau_id, txn1_root, "/A/D/G/tau", pool));
    SVN_ERR (svn_fs_node_id (&t2_tau_id, txn2_root, "/A/D/G/tau", pool));

    /* Abort just txn2. */
    SVN_ERR (svn_fs_abort_txn (txn2));

    /* Now test that all the nodes in txn2 at the time of the abort
     * are gone, but all of the ones in txn1 are still there. 
     */

    /* Check that every node rev in t2 has vanished from the fs. */
    SVN_ERR (check_id_absent (fs, t2_root_id));
    SVN_ERR (check_id_absent (fs, t2_iota_id));
    SVN_ERR (check_id_absent (fs, t2_A_id));
    SVN_ERR (check_id_absent (fs, t2_mu_id));
    SVN_ERR (check_id_absent (fs, t2_B_id));
    SVN_ERR (check_id_absent (fs, t2_lambda_id));
    SVN_ERR (check_id_absent (fs, t2_E_id));
    SVN_ERR (check_id_absent (fs, t2_alpha_id));
    SVN_ERR (check_id_absent (fs, t2_beta_id));
    SVN_ERR (check_id_absent (fs, t2_F_id));
    SVN_ERR (check_id_absent (fs, t2_C_id));
    SVN_ERR (check_id_absent (fs, t2_D_id));
    SVN_ERR (check_id_absent (fs, t2_gamma_id));
    SVN_ERR (check_id_absent (fs, t2_H_id));
    SVN_ERR (check_id_absent (fs, t2_chi_id));
    SVN_ERR (check_id_absent (fs, t2_psi_id));
    SVN_ERR (check_id_absent (fs, t2_omega_id));
    SVN_ERR (check_id_absent (fs, t2_G_id));
    SVN_ERR (check_id_absent (fs, t2_pi_id));
    SVN_ERR (check_id_absent (fs, t2_rho_id));
    SVN_ERR (check_id_absent (fs, t2_tau_id));
    
    /* Check that every node rev in t1 is still in the fs. */
    SVN_ERR (check_id_present (fs, t1_root_id));
    SVN_ERR (check_id_present (fs, t1_iota_id));
    SVN_ERR (check_id_present (fs, t1_A_id));
    SVN_ERR (check_id_present (fs, t1_mu_id));
    SVN_ERR (check_id_present (fs, t1_B_id));
    SVN_ERR (check_id_present (fs, t1_lambda_id));
    SVN_ERR (check_id_present (fs, t1_E_id));
    SVN_ERR (check_id_present (fs, t1_alpha_id));
    SVN_ERR (check_id_present (fs, t1_beta_id));
    SVN_ERR (check_id_present (fs, t1_F_id));
    SVN_ERR (check_id_present (fs, t1_C_id));
    SVN_ERR (check_id_present (fs, t1_D_id));
    SVN_ERR (check_id_present (fs, t1_gamma_id));
    SVN_ERR (check_id_present (fs, t1_H_id));
    SVN_ERR (check_id_present (fs, t1_chi_id));
    SVN_ERR (check_id_present (fs, t1_psi_id));
    SVN_ERR (check_id_present (fs, t1_omega_id));
    SVN_ERR (check_id_present (fs, t1_G_id));
    SVN_ERR (check_id_present (fs, t1_pi_id));
    SVN_ERR (check_id_present (fs, t1_rho_id));
    SVN_ERR (check_id_present (fs, t1_tau_id));
  }

  /* Test that txn2 itself is gone, by trying to open it. */
  {
    svn_fs_txn_t *txn2_again;
    svn_error_t *err;

    err = svn_fs_open_txn (&txn2_again, fs, txn2_name, pool);
    if (err && (err->apr_err != SVN_ERR_FS_NO_SUCH_TRANSACTION))
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "opening non-existent txn got wrong error");
      }
    else if (! err)
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "opening non-existent txn failed to get error");
      }
  }

  /* Test that txn names are not recycled, by opening a new txn.  */
  {
    svn_fs_txn_t *txn3;
    const char *txn3_name;

    SVN_ERR (svn_fs_begin_txn (&txn3, fs, 0, pool));
    SVN_ERR (svn_fs_txn_name (&txn3_name, txn3, pool));

    if ((strcmp (txn3_name, txn2_name) == 0)
        || (strcmp (txn3_name, txn1_name) == 0))
      {
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "txn name \"%s\" was recycled", txn3_name);
      }

    SVN_ERR (svn_fs_close_txn (txn3));
  }

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn1));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


/* Attempt a merge using arguments 2 through N.  If EXPECTED_CONFLICT
   is null, return an error if there is any indication of a conflict
   having happened.  Else EXPECTED_CONFLICT is a string representing
   the expected conflict path; but if it is the empty string, then any
   conflict is acceptable.

   If the merge appeared to have inconsistent results, such as
   flagging no conflict but failing to set the conflict information
   pointer to null, then this function returns an error. */
static svn_error_t *
attempt_merge (const char *expected_conflict,
               svn_fs_root_t *source_root,
               const char *source_path,
               svn_fs_root_t *target_root,
               const char *target_path,
               svn_fs_root_t *ancestor_root,
               const char *ancestor_path,
               apr_pool_t *subpool)
{
  svn_error_t *err;
  const char *conflict;

  err = svn_fs_merge (&conflict,
                      source_root, source_path,
                      target_root, target_path,
                      ancestor_root, ancestor_path,
                      subpool);

  if (err && (err->apr_err == SVN_ERR_FS_CONFLICT))
    {
      if (! expected_conflict)
        {
          return svn_error_createf
            (SVN_ERR_FS_GENERAL, 0, NULL, subpool,
             "merge did not expect conflict, but got conflict at `%s'",
             conflict);
        }
      else if ((strcmp (expected_conflict, "") != 0)
               && (strcmp (expected_conflict, conflict) != 0))
        {
          return svn_error_createf
            (SVN_ERR_FS_GENERAL, 0, NULL, subpool,
             "merge expected conflict `%s', but got conflict `%s'", 
             expected_conflict, conflict);
        }
    }
  else if (err)
    {
      /* A non-conflict error.  Just return it unconditionally. */
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, subpool,
         "merge resulted in non-conflict error");
    }
  else  /* no error */
    {
      if (expected_conflict)
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, subpool,
           "merge expected to conflict, but did not");
    }

  return SVN_NO_ERROR;
}


/* Test svn_fs_merge(). */
static svn_error_t *
merge_trees (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *source_txn, *target_txn, *ancestor_txn;
  svn_fs_root_t *source_root, *target_root, *ancestor_root;
  const char
    *source_path = "",
    *target_path = "",
    *ancestor_path = "";

  *msg = "merge trees";

  /* Prepare three txns to receive a greek tree each. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-merge-trees"));
  SVN_ERR (svn_fs_begin_txn (&source_txn, fs, 0, pool));
  SVN_ERR (svn_fs_begin_txn (&target_txn, fs, 0, pool));
  SVN_ERR (svn_fs_begin_txn (&ancestor_txn, fs, 0, pool));

  /* Make roots. */
  SVN_ERR (svn_fs_txn_root (&source_root, source_txn, pool));
  SVN_ERR (svn_fs_txn_root (&target_root, target_txn, pool));
  SVN_ERR (svn_fs_txn_root (&ancestor_root, ancestor_txn, pool));

  /* Test #1: Empty, unmodified trees should not conflict. */
  SVN_ERR (attempt_merge (NULL,
                          source_root, source_path,
                          target_root, target_path,
                          ancestor_root, ancestor_path,
                          pool));

  /* Test #2: Non-conflicting changes in greek trees in txns.  Should
   * still get a conflict, because none of them are committed trees,
   * therefore no txn's tree shares any node revision IDs with another
   * txn tree.
   * 
   * Leave ANCESTOR alone.  In TARGET,
   *
   *    - add /target_theta
   *    - add /A/D/target_zeta
   *    - del /A/D/G/pi
   *    - del /A/D/H/omega
   * 
   * In SOURCE,
   *
   *    - add /source_theta
   *    - add /A/D/source_zeta
   *    - del /A/D/gamma
   *    - del /A/D/G/rho
   *    - del /A/D/H/chi
   *    - del /A/D/H/psi
   * 
   * Note that after the merge, two of three files in /A/D/G/ should
   * be gone, leaving only tau, but all three files in /A/D/H/ will be
   * gone, leaving H an empty directory.
   */
  
  /* Create greek trees. */
  SVN_ERR (greek_tree_under_root (source_root));
  SVN_ERR (greek_tree_under_root (target_root));
  SVN_ERR (greek_tree_under_root (ancestor_root));

  /* Do some things in target. */
  SVN_ERR (svn_fs_make_file (target_root, "target_theta", pool));
  SVN_ERR (svn_fs_make_file (target_root, "A/D/target_zeta", pool));
  SVN_ERR (svn_fs_delete (target_root, "A/D/G/pi", pool));
  SVN_ERR (svn_fs_delete (target_root, "A/D/H/omega", pool));

  /* Do some things in source. */
  SVN_ERR (svn_fs_make_file (source_root, "source_theta", pool));
  SVN_ERR (svn_fs_make_file (source_root, "A/D/source_zeta", pool));
  SVN_ERR (svn_fs_delete (source_root, "A/D/gamma", pool));
  SVN_ERR (svn_fs_delete (source_root, "A/D/G/rho", pool));
  SVN_ERR (svn_fs_delete (source_root, "A/D/H/chi", pool));
  SVN_ERR (svn_fs_delete (source_root, "A/D/H/psi", pool));

  /* We attempt to merge, knowing we will get a conflict.  Even though
     the contents of the trees are exactly the same, the node revision
     IDs are all different, because these are three transactions.  */

  SVN_ERR (attempt_merge ("",
                          source_root, source_path,
                          target_root, target_path,
                          ancestor_root, ancestor_path,
                          pool));

  /* Test #3-N: should go through the cases enumerated svn_fs_merge,
     cook up a scenario for each one.  But we'll need commits working
     to really do this right.  */

  /* ### kff todo: hmmm, and now merging_commit() does these tests.
     Is this function obsolete? */

  return SVN_NO_ERROR;
}


/* Commit TXN, expecting either success or failure:
 *
 * If EXPECTED_CONFLICT is null, then the commit is expected to
 * succeed.  If it does succeed, set *NEW_REV to the new revision;
 * else return error.
 *
 * If EXPECTED_CONFLICT is non-null, it is either the empty string or
 * the expected path of the conflict.  If it is the empty string, any
 * conflict is acceptable.  If it is a non-empty string, the commit
 * must fail due to conflict, and the conflict path must match
 * EXPECTED_CONFLICT.  If they don't match, return error.
 *
 * If a conflict is expected but the commit succeeds anyway, return
 * error.
 */
static svn_error_t *
test_commit_txn (svn_revnum_t *new_rev,
                 svn_fs_txn_t *txn,
                 const char *expected_conflict)
{
  const char *conflict;
  svn_error_t *err;

  err = svn_fs_commit_txn (&conflict, new_rev, txn);

  if (err && (err->apr_err == SVN_ERR_FS_CONFLICT))
    {
      if (! expected_conflict)
        {
          return svn_error_createf
            (SVN_ERR_FS_CONFLICT, 0, NULL, pool,
             "commit conflicted at `%s', but no conflict expected",
             conflict ? conflict : "(missing conflict info!)");
        }
      else if (conflict == NULL)
        {
          return svn_error_createf
            (SVN_ERR_FS_CONFLICT, 0, NULL, pool,
             "commit conflicted as expected, "
             "but no conflict path was returned (`%s' expected)",
             expected_conflict);
        }
      else if ((strcmp (expected_conflict, "") != 0)
               && (strcmp (conflict, expected_conflict) != 0))
        {
          return svn_error_createf
            (SVN_ERR_FS_CONFLICT, 0, NULL, pool,
             "commit conflicted at `%s', but expected conflict at `%s')",
             conflict, expected_conflict);
        }
    }
  else if (err)   /* commit failed, but not due to conflict */
    {
      return svn_error_quick_wrap 
        (err, "commit failed due to something other than a conflict");
    }
  else            /* err == NULL, so commit succeeded */
    {
      if (expected_conflict)
        {
          return svn_error_createf
            (SVN_ERR_FS_GENERAL, 0, NULL, pool,
             "commit succeeded that was expected to fail at `%s'",
             expected_conflict);
        }
    }

  return SVN_NO_ERROR;
}


/* Fetch the youngest revision from a repos. */
static svn_error_t *
fetch_youngest_rev (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  svn_revnum_t new_rev;
  svn_revnum_t youngest_rev, new_youngest_rev;

  *msg = "fetch the youngest revision from a filesystem";

  SVN_ERR (create_fs_and_repos (&fs, "test-repo-youngest-rev"));

  /* Get youngest revision of brand spankin' new filesystem. */
  SVN_ERR (svn_fs_youngest_rev (&youngest_rev, fs, pool));

  /* Prepare a txn to receive the greek tree. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-commit-txn"));
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree. */
  SVN_ERR (greek_tree_under_root (txn_root));

  /* Commit it. */
  SVN_ERR (test_commit_txn (&new_rev, txn, NULL));

  /* Get the new youngest revision. */
  SVN_ERR (svn_fs_youngest_rev (&new_youngest_rev, fs, pool));

  if (youngest_rev == new_rev)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "commit didn't bump up revision number");

  if (new_youngest_rev != new_rev)
    return svn_error_create (SVN_ERR_FS_GENERAL, 0, NULL, pool,
                             "couldn't fetch youngest revision");

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


/* Test committing against an empty repository.
   todo: also test committing against youngest? */
static svn_error_t *
basic_commit (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root, *revision_root;
  svn_revnum_t before_rev, after_rev;
  const char *conflict;

  *msg = "basic commit";

  /* Prepare a filesystem. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-basic-commit"));

  /* Save the current youngest revision. */
  SVN_ERR (svn_fs_youngest_rev (&before_rev, fs, pool));

  /* Prepare a txn to receive the greek tree. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Paranoidly check that the current youngest rev is unchanged. */
  SVN_ERR (svn_fs_youngest_rev (&after_rev, fs, pool));
  if (after_rev != before_rev)
    return svn_error_create
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "youngest revision changed unexpectedly");

  /* Create the greek tree. */
  SVN_ERR (greek_tree_under_root (txn_root));

  /* Commit it. */
  SVN_ERR (svn_fs_commit_txn (&conflict, &after_rev, txn));

  /* Close the transaction */
  SVN_ERR (svn_fs_close_txn (txn));

  /* Make sure it's a different revision than before. */
  if (after_rev == before_rev)
    return svn_error_create
      (SVN_ERR_FS_GENERAL, 0, NULL, pool,
       "youngest revision failed to change");

  /* Get root of the revision */
  SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool));

  /* Check the tree. */
  SVN_ERR (check_greek_tree_under_root (revision_root));

  /* Close the fs. */
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}



static svn_error_t *
test_tree_node_validation (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root, *revision_root;
  svn_revnum_t after_rev;
  const char *conflict;

  *msg = "testing tree validation helper";

  /* Prepare a filesystem. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-validate-tree-entries"));

  /* In a txn, create the greek tree. */
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "iota",        "This is the file 'iota'.\n" },
      { "A",           0 },
      { "A/mu",        "This is the file 'mu'.\n" },
      { "A/B",         0 },
      { "A/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E",       0 },
      { "A/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/B/F",       0 },
      { "A/C",         0 },
      { "A/D",         0 },
      { "A/D/gamma",   "This is the file 'gamma'.\n" },
      { "A/D/G",       0 },
      { "A/D/G/pi",    "This is the file 'pi'.\n" },
      { "A/D/G/rho",   "This is the file 'rho'.\n" },
      { "A/D/G/tau",   "This is the file 'tau'.\n" },
      { "A/D/H",       0 },
      { "A/D/H/chi",   "This is the file 'chi'.\n" },
      { "A/D/H/psi",   "This is the file 'psi'.\n" },
      { "A/D/H/omega", "This is the file 'omega'.\n" }
    };

    SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (greek_tree_under_root (txn_root));

    /* Carefully validate that tree in the transaction. */
    SVN_ERR (validate_tree (txn_root, expected_entries, 20));

    /* Go ahead and commit the tree */
    SVN_ERR (svn_fs_commit_txn (&conflict, &after_rev, txn));
    SVN_ERR (svn_fs_close_txn (txn));

    /* Carefully validate that tree in the new revision, now. */
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool));
    SVN_ERR (validate_tree (revision_root, expected_entries, 20));
  }

  /* In a new txn, modify the greek tree. */
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "iota",          "This is a new version of 'iota'.\n" },
      { "A",             0 },
      { "A/B",           0 },
      { "A/B/lambda",    "This is the file 'lambda'.\n" },
      { "A/B/E",         0 },
      { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
      { "A/B/E/beta",    "This is the file 'beta'.\n" },
      { "A/B/F",         0 },
      { "A/C",           0 },
      { "A/C/kappa",     "This is the file 'kappa'.\n" },
      { "A/D",           0 },
      { "A/D/gamma",     "This is the file 'gamma'.\n" },
      { "A/D/H",         0 },
      { "A/D/H/chi",     "This is the file 'chi'.\n" },
      { "A/D/H/psi",     "This is the file 'psi'.\n" },
      { "A/D/H/omega",   "This is the file 'omega'.\n" },
      { "A/D/I",         0 },
      { "A/D/I/delta",   "This is the file 'delta'.\n" },
      { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
    };

    SVN_ERR (svn_fs_begin_txn (&txn, fs, after_rev, pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (set_file_contents (txn_root, "iota", 
                                "This is a new version of 'iota'.\n"));
    SVN_ERR (svn_fs_delete (txn_root, "A/mu", pool));            
    SVN_ERR (svn_fs_delete_tree (txn_root, "A/D/G", pool));            
    SVN_ERR (svn_fs_make_dir (txn_root, "A/D/I", pool));
    SVN_ERR (svn_fs_make_file (txn_root, "A/D/I/delta", pool));
    SVN_ERR (set_file_contents (txn_root, "A/D/I/delta",
                                "This is the file 'delta'.\n"));
    SVN_ERR (svn_fs_make_file (txn_root, "A/D/I/epsilon", pool));
    SVN_ERR (set_file_contents (txn_root, "A/D/I/epsilon",
                                "This is the file 'epsilon'.\n"));
    SVN_ERR (svn_fs_make_file (txn_root, "A/C/kappa", pool));
    SVN_ERR (set_file_contents (txn_root, "A/C/kappa",
                                "This is the file 'kappa'.\n"));

    /* Carefully validate that tree in the transaction. */
    SVN_ERR (validate_tree (txn_root, expected_entries, 19));
    
    /* Go ahead and commit the tree */
    SVN_ERR (svn_fs_commit_txn (&conflict, &after_rev, txn));
    SVN_ERR (svn_fs_close_txn (txn));

    /* Carefully validate that tree in the new revision, now. */
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool));
    SVN_ERR (validate_tree (revision_root, expected_entries, 19));
  }

  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


static svn_error_t *
fetch_by_id (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root, *revision_root, *id_root;
  svn_revnum_t after_rev;
  svn_error_t *err;

  *msg = "fetch by id";

  /* Commit a Greek Tree as the first revision. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-fetch-by-id"));
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (greek_tree_under_root (txn_root));
  SVN_ERR (svn_fs_commit_txn (NULL, &after_rev, txn));
  SVN_ERR (svn_fs_close_txn (txn));

  /* Get one root for the committed Greek Tree, one for the fs. */
  SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool));
  SVN_ERR (svn_fs_id_root (&id_root, fs, pool));

  /* Get the IDs of some random paths, then fetch some content by ID. */
  {
    svn_fs_id_t *iota_id, *beta_id, *C_id, *D_id, *omega_id;
    svn_string_t *iota_str, *beta_str, *C_str, *D_str, *omega_str;
    svn_string_t *not_an_id_str = svn_string_create ("fish", pool);
    apr_hash_t *entries;
    apr_off_t len;
    void *val;
    int is;

    SVN_ERR (svn_fs_node_id (&iota_id, revision_root, "iota", pool));
    SVN_ERR (svn_fs_node_id (&beta_id, revision_root, "A/B/E/beta", pool));
    SVN_ERR (svn_fs_node_id (&C_id, revision_root, "A/C", pool));
    SVN_ERR (svn_fs_node_id (&D_id, revision_root, "A/D", pool));
    SVN_ERR (svn_fs_node_id (&omega_id, revision_root, "A/D/H/omega", pool));
  
    iota_str  = svn_fs_unparse_id (iota_id, pool);
    beta_str  = svn_fs_unparse_id (beta_id, pool);
    C_str     = svn_fs_unparse_id (C_id, pool);
    D_str     = svn_fs_unparse_id (D_id, pool);
    omega_str = svn_fs_unparse_id (omega_id, pool);

    /* Check iota. */
    SVN_ERR (svn_fs_is_dir (&is, id_root, iota_str->data, pool));
    if (is)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "file fetched by node claimed to be a directory");

    SVN_ERR (svn_fs_is_file (&is, id_root, iota_str->data, pool));
    if (! is)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "file fetched by node claimed not to be a file");

    SVN_ERR (svn_fs_is_different (&is, revision_root, "iota",
                                  id_root, iota_str->data, pool));
    if (is)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "fetching file by path and by node got different results");


    /* Check D. */
    SVN_ERR (svn_fs_is_file (&is, id_root, D_str->data, pool));
    if (is)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "dir fetched by node claimed to be a file");

    SVN_ERR (svn_fs_is_dir (&is, id_root, D_str->data, pool));
    if (! is)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "dir fetched by node claimed not to be a dir");

    SVN_ERR (svn_fs_is_different (&is, revision_root, "A/D",
                                  id_root, D_str->data, pool));
    if (is)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "fetching dir by path and by node got different results");


    SVN_ERR (svn_fs_dir_entries (&entries, id_root, D_str->data, pool));
    val = apr_hash_get (entries, "gamma", APR_HASH_KEY_STRING);
    if (! val)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "dir fetched by id doesn't have expected entry \"gamma\"");

    val = apr_hash_get (entries, "G", APR_HASH_KEY_STRING);
    if (! val)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "dir fetched by id doesn't have expected entry \"G\"");

    val = apr_hash_get (entries, "H", APR_HASH_KEY_STRING);
    if (! val)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "dir fetched by id doesn't have expected entry \"H\"");

    if (apr_hash_count (entries) != 3)
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "dir fetched by id has unexpected number of entries");
      

    /* Check omega. */
    SVN_ERR (svn_fs_file_length (&len, id_root, omega_str->data, pool));
    if (len != strlen ("This is the file 'omega'.\n"))
      return svn_error_create
        (SVN_ERR_FS_GENERAL, 0, NULL, pool,
         "file fetched by id has wrong length");
      
    {
      svn_stream_t *contents_stream;
      svn_string_t *contents_string;

      SVN_ERR (svn_fs_file_contents (&contents_stream, id_root,
                                     omega_str->data, pool));
      SVN_ERR (stream_to_string (&contents_string, contents_stream));

      if (strcmp (contents_string->data, "This is the file 'omega'.\n") != 0)
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "file fetched by had wrong contents");
    }


    /* Try fetching a non-ID. */
    err = svn_fs_file_length (&len, id_root, not_an_id_str->data, pool);
    if (! err)
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "fetching an invalid id should fail, but did not");
      }
    else if (err->apr_err != SVN_ERR_FS_NOT_ID)
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "fetching an invalid id failed with the wrong error");
      }


    /* Try changing a node fetched by ID. */
    err = svn_fs_delete (id_root, C_str->data, pool);
    if (! err)
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "deleting an ID path should fail, but did not");
      }
    
  }

  return SVN_NO_ERROR;
}


/* Commit with merging (committing against non-youngest). */ 
static svn_error_t *
merging_commit (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root, *revision_root;
  svn_revnum_t after_rev;
  svn_revnum_t revisions[48];
  int i;
  int revision_count;

  *msg = "merging commit";

  /* Initialize our revision number stuffs. */
  for (i = 0;
       i < ((sizeof (revisions)) / (sizeof (svn_revnum_t)));
       i++)
    revisions[i] = SVN_INVALID_REVNUM;
  revision_count = 0;

  /* Prepare a filesystem. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-merging-commit"));
  revisions[revision_count++] = 0; /* the brand spankin' new revision */

  /***********************************************************************/
  /* REVISION 0 */
  /***********************************************************************/

  /* In one txn, create and commit the greek tree. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (greek_tree_under_root (txn_root));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));

  /***********************************************************************/
  /* REVISION 1 */
  /***********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "iota",        "This is the file 'iota'.\n" },
      { "A",           0 },
      { "A/mu",        "This is the file 'mu'.\n" },
      { "A/B",         0 },
      { "A/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E",       0 },
      { "A/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/B/F",       0 },
      { "A/C",         0 },
      { "A/D",         0 },
      { "A/D/gamma",   "This is the file 'gamma'.\n" },
      { "A/D/G",       0 },
      { "A/D/G/pi",    "This is the file 'pi'.\n" },
      { "A/D/G/rho",   "This is the file 'rho'.\n" },
      { "A/D/G/tau",   "This is the file 'tau'.\n" },
      { "A/D/H",       0 },
      { "A/D/H/chi",   "This is the file 'chi'.\n" },
      { "A/D/H/psi",   "This is the file 'psi'.\n" },
      { "A/D/H/omega", "This is the file 'omega'.\n" }
    };
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
    SVN_ERR (validate_tree (revision_root, expected_entries, 20));
  }
  SVN_ERR (svn_fs_close_txn (txn));
  revisions[revision_count++] = after_rev;

  /* Let's add a directory and some files to the tree, and delete 
     'iota' */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[revision_count-1], pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_make_dir (txn_root, "A/D/I", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/I/delta", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/I/delta",
                              "This is the file 'delta'.\n"));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/I/epsilon", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/I/epsilon",
                              "This is the file 'epsilon'.\n"));
  SVN_ERR (svn_fs_make_file (txn_root, "A/C/kappa", pool));
  SVN_ERR (set_file_contents (txn_root, "A/C/kappa",
                              "This is the file 'kappa'.\n"));
  SVN_ERR (svn_fs_delete (txn_root, "iota", pool));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));

  /***********************************************************************/
  /* REVISION 2 */
  /***********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "A",             0 },
      { "A/mu",          "This is the file 'mu'.\n" },
      { "A/B",           0 },
      { "A/B/lambda",    "This is the file 'lambda'.\n" },
      { "A/B/E",         0 },
      { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
      { "A/B/E/beta",    "This is the file 'beta'.\n" },
      { "A/B/F",         0 },
      { "A/C",           0 },
      { "A/C/kappa",     "This is the file 'kappa'.\n" },
      { "A/D",           0 },
      { "A/D/gamma",     "This is the file 'gamma'.\n" },
      { "A/D/G",         0 },
      { "A/D/G/pi",      "This is the file 'pi'.\n" },
      { "A/D/G/rho",     "This is the file 'rho'.\n" },
      { "A/D/G/tau",     "This is the file 'tau'.\n" },
      { "A/D/H",         0 },
      { "A/D/H/chi",     "This is the file 'chi'.\n" },
      { "A/D/H/psi",     "This is the file 'psi'.\n" },
      { "A/D/H/omega",   "This is the file 'omega'.\n" },
      { "A/D/I",         0 },
      { "A/D/I/delta",   "This is the file 'delta'.\n" },
      { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
    };
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
    SVN_ERR (validate_tree (revision_root, expected_entries, 23));
  }
  SVN_ERR (svn_fs_close_txn (txn));
  revisions[revision_count++] = after_rev;

  /* We don't think the A/D/H directory is pulling it's weight...let's
     knock it off.  Oh, and let's re-add iota, too. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[revision_count-1], pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_delete_tree (txn_root, "A/D/H", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "iota", pool));
  SVN_ERR (set_file_contents (txn_root, "iota",
                              "This is the new file 'iota'.\n"));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));

  /***********************************************************************/
  /* REVISION 3 */
  /***********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "iota",          "This is the new file 'iota'.\n" },
      { "A",             0 },
      { "A/mu",          "This is the file 'mu'.\n" },
      { "A/B",           0 },
      { "A/B/lambda",    "This is the file 'lambda'.\n" },
      { "A/B/E",         0 },
      { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
      { "A/B/E/beta",    "This is the file 'beta'.\n" },
      { "A/B/F",         0 },
      { "A/C",           0 },
      { "A/C/kappa",     "This is the file 'kappa'.\n" },
      { "A/D",           0 },
      { "A/D/gamma",     "This is the file 'gamma'.\n" },
      { "A/D/G",         0 },
      { "A/D/G/pi",      "This is the file 'pi'.\n" },
      { "A/D/G/rho",     "This is the file 'rho'.\n" },
      { "A/D/G/tau",     "This is the file 'tau'.\n" },
      { "A/D/I",         0 },
      { "A/D/I/delta",   "This is the file 'delta'.\n" },
      { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
    };
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
    SVN_ERR (validate_tree (revision_root, expected_entries, 20));
  }
  SVN_ERR (svn_fs_close_txn (txn));
  revisions[revision_count++] = after_rev;

  /* Delete iota (yet again). */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[revision_count-1], pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_delete (txn_root, "iota", pool)); 
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));

  /***********************************************************************/
  /* REVISION 4 */
  /***********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "A",             0 },
      { "A/mu",          "This is the file 'mu'.\n" },
      { "A/B",           0 },
      { "A/B/lambda",    "This is the file 'lambda'.\n" },
      { "A/B/E",         0 },
      { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
      { "A/B/E/beta",    "This is the file 'beta'.\n" },
      { "A/B/F",         0 },
      { "A/C",           0 },
      { "A/C/kappa",     "This is the file 'kappa'.\n" },
      { "A/D",           0 },
      { "A/D/gamma",     "This is the file 'gamma'.\n" },
      { "A/D/G",         0 },
      { "A/D/G/pi",      "This is the file 'pi'.\n" },
      { "A/D/G/rho",     "This is the file 'rho'.\n" },
      { "A/D/G/tau",     "This is the file 'tau'.\n" },
      { "A/D/I",         0 },
      { "A/D/I/delta",   "This is the file 'delta'.\n" },
      { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
    };
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
    SVN_ERR (validate_tree (revision_root, expected_entries, 19));
  }
  SVN_ERR (svn_fs_close_txn (txn));
  revisions[revision_count++] = after_rev;

  /***********************************************************************/
  /* GIVEN:  A and B, with common ancestor ANCESTOR, where A and B
     directories, and E, an entry in either A, B, or ANCESTOR.

     For every E, the following cases exist:
      - E exists in neither ANCESTOR nor A.
      - E doesn't exist in ANCESTOR, and has been added to A.
      - E exists in ANCESTOR, but has been deleted from A.
      - E exists in both ANCESTOR and A ...
        - but refers to different node revisions.
        - and refers to the same node revision.

     The same set of possible relationships with ANCESTOR holds for B,
     so there are thirty-six combinations.  The matrix is symmetrical
     with A and B reversed, so we only have to describe one triangular
     half, including the diagonal --- 21 combinations.

     Our goal here is to test all the possible scenarios that can
     occur given the above boolean logic table, and to make sure that
     the results we get are as expected.  

     The test cases below have the following features:

     - They run straight through the scenarios as described in the
       `structure' document at this time.

     - In each case, a txn is begun based on some revision (ANCESTOR),
       is modified into a new tree (B), and then is attempted to be
       committed (which happens against the head of the tree, A).

     - If the commit is successful (and is *expected* to be such),
       that new revision (which exists now as a result of the
       successful commit) is thoroughly tested for accuracy of tree
       entries, and in the case of files, for their contents.  It is
       important to realize that these successful commits are
       advancing the head of the tree, and each one effective becomes
       the new `A' described in further test cases.
  */
  /***********************************************************************/

  /* (6) E exists in neither ANCESTOR nor A. */
  {
    /* (1) E exists in neither ANCESTOR nor B.  Can't occur, by
       assumption that E exists in either A, B, or ancestor. */

    /* (1) E has been added to B.  Add E in the merged result. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[0], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (svn_fs_make_file (txn_root, "theta", pool));
    SVN_ERR (set_file_contents (txn_root, "theta",
                                "This is the file 'theta'.\n"));
    SVN_ERR (test_commit_txn (&after_rev, txn, NULL));

    /*********************************************************************/
    /* REVISION 5 */
    /*********************************************************************/
    {
      tree_test_entry_t expected_entries[] = {
        /* path, contents (0 = dir) */
        { "theta",         "This is the file 'theta'.\n" },
        { "A",             0 },
        { "A/mu",          "This is the file 'mu'.\n" },
        { "A/B",           0 },
        { "A/B/lambda",    "This is the file 'lambda'.\n" },
        { "A/B/E",         0 },
        { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
        { "A/B/E/beta",    "This is the file 'beta'.\n" },
        { "A/B/F",         0 },
        { "A/C",           0 },
        { "A/C/kappa",     "This is the file 'kappa'.\n" },
        { "A/D",           0 },
        { "A/D/gamma",     "This is the file 'gamma'.\n" },
        { "A/D/G",         0 },
        { "A/D/G/pi",      "This is the file 'pi'.\n" },
        { "A/D/G/rho",     "This is the file 'rho'.\n" },
        { "A/D/G/tau",     "This is the file 'tau'.\n" },
        { "A/D/I",         0 },
        { "A/D/I/delta",   "This is the file 'delta'.\n" },
        { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
      };
      SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
      SVN_ERR (validate_tree (revision_root, expected_entries, 20));
    }
    revisions[revision_count++] = after_rev;

    /* (1) E has been deleted from B.  Can't occur, by assumption that
       E doesn't exist in ANCESTOR. */

    /* (3) E exists in both ANCESTOR and B.  Can't occur, by
       assumption that E doesn't exist in ancestor. */
  }

  /* (5) E doesn't exist in ANCESTOR, and has been added to A. */
  {
    /* (1) E doesn't exist in ANCESTOR, and has been added to B.
       Conflict. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[4], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (svn_fs_make_file (txn_root, "theta", pool));
    SVN_ERR (set_file_contents (txn_root, "theta",
                                "This is another file 'theta'.\n"));
    SVN_ERR (test_commit_txn (&after_rev, txn, "/theta"));

    /* (1) E exists in ANCESTOR, but has been deleted from B.  Can't
       occur, by assumption that E doesn't exist in ANCESTOR. */

    /* (3) E exists in both ANCESTOR and B.  Can't occur, by assumption
       that E doesn't exist in ANCESTOR. */
  }

  /* (4) E exists in ANCESTOR, but has been deleted from A */
  {
    /* (1) E exists in ANCESTOR, but has been deleted from B.  If
       neither delete was a result of a rename, then omit E from the
       merged tree.  Otherwise, conflict. */
    /* ### cmpilato todo: the rename case isn't actually handled by
       merge yet, so we know we won't get a conflict here. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (svn_fs_delete_tree (txn_root, "A/D/H", pool));
    SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
    /*********************************************************************/
    /* REVISION 6 */
    /*********************************************************************/
    {
      tree_test_entry_t expected_entries[] = {
        /* path, contents (0 = dir) */
        { "theta",         "This is the file 'theta'.\n" },
        { "A",             0 },
        { "A/mu",          "This is the file 'mu'.\n" },
        { "A/B",           0 },
        { "A/B/lambda",    "This is the file 'lambda'.\n" },
        { "A/B/E",         0 },
        { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
        { "A/B/E/beta",    "This is the file 'beta'.\n" },
        { "A/B/F",         0 },
        { "A/C",           0 },
        { "A/C/kappa",     "This is the file 'kappa'.\n" },
        { "A/D",           0 },
        { "A/D/gamma",     "This is the file 'gamma'.\n" },
        { "A/D/G",         0 },
        { "A/D/G/pi",      "This is the file 'pi'.\n" },
        { "A/D/G/rho",     "This is the file 'rho'.\n" },
        { "A/D/G/tau",     "This is the file 'tau'.\n" },
        { "A/D/I",         0 },
        { "A/D/I/delta",   "This is the file 'delta'.\n" },
        { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
      };
      SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
      SVN_ERR (validate_tree (revision_root, expected_entries, 20));
    }
    revisions[revision_count++] = after_rev;

    /* Try deleting a file F inside a subtree S where S does not exist
       in the most recent revision, but does exist in the ancestor
       tree.  This should conflict. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (svn_fs_delete (txn_root, "A/D/H/omega", pool));
    SVN_ERR (test_commit_txn (&after_rev, txn, "/A/D/H"));

    /* E exists in both ANCESTOR and B ... */
    {
      /* (1) but refers to different nodes.  Conflict. */
      SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
      SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
      SVN_ERR (svn_fs_delete_tree (txn_root, "A/D/H", pool));
      SVN_ERR (svn_fs_make_dir (txn_root, "A/D/H", pool));
      SVN_ERR (test_commit_txn (&after_rev, txn, "/A/D/H"));

      /* (1) but refers to different revisions of the same node.
         Conflict. */
      SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
      SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
      SVN_ERR (svn_fs_make_file (txn_root, "A/D/H/zeta", pool));
      SVN_ERR (test_commit_txn (&after_rev, txn, "/A/D/H"));

      /* (1) and refers to the same node revision.  Omit E from the
         merged tree.  This is already tested in Merge-Test 3
         (A/D/H/chi, A/D/H/psi, e.g.), but we'll test it here again
         anyway.  A little paranoia never hurt anyone.  */
      SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
      SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
      SVN_ERR (svn_fs_delete (txn_root, "A/mu", pool)); /* unrelated change */
      SVN_ERR (test_commit_txn (&after_rev, txn, NULL));

      /*********************************************************************/
      /* REVISION 7 */
      /*********************************************************************/
      {
        tree_test_entry_t expected_entries[] = {
          /* path, contents (0 = dir) */
          { "theta",         "This is the file 'theta'.\n" },
          { "A",             0 },
          { "A/B",           0 },
          { "A/B/lambda",    "This is the file 'lambda'.\n" },
          { "A/B/E",         0 },
          { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
          { "A/B/E/beta",    "This is the file 'beta'.\n" },
          { "A/B/F",         0 },
          { "A/C",           0 },
          { "A/C/kappa",     "This is the file 'kappa'.\n" },
          { "A/D",           0 },
          { "A/D/gamma",     "This is the file 'gamma'.\n" },
          { "A/D/G",         0 },
          { "A/D/G/pi",      "This is the file 'pi'.\n" },
          { "A/D/G/rho",     "This is the file 'rho'.\n" },
          { "A/D/G/tau",     "This is the file 'tau'.\n" },
          { "A/D/I",         0 },
          { "A/D/I/delta",   "This is the file 'delta'.\n" },
          { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
        };
        SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
        SVN_ERR (validate_tree (revision_root, expected_entries, 19));
      }
      revisions[revision_count++] = after_rev;
    }
  }

  /* Preparation for upcoming tests. 
     We make a new head revision, with A/mu restored, but containing
     slightly different contents than its first incarnation. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[revision_count-1], pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_make_file (txn_root, "A/mu", pool));
  SVN_ERR (set_file_contents (txn_root, "A/mu", "A new file 'mu'.\n"));
  SVN_ERR (svn_fs_make_file (txn_root, "A/D/G/xi", pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/G/xi",
                              "This is the file 'xi'.\n"));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
  /*********************************************************************/
  /* REVISION 8 */
  /*********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "theta",         "This is the file 'theta'.\n" },
      { "A",             0 },
      { "A/mu",          "A new file 'mu'.\n" },
      { "A/B",           0 },
      { "A/B/lambda",    "This is the file 'lambda'.\n" },
      { "A/B/E",         0 },
      { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
      { "A/B/E/beta",    "This is the file 'beta'.\n" },
      { "A/B/F",         0 },
      { "A/C",           0 },
      { "A/C/kappa",     "This is the file 'kappa'.\n" },
      { "A/D",           0 },
      { "A/D/gamma",     "This is the file 'gamma'.\n" },
      { "A/D/G",         0 },
      { "A/D/G/pi",      "This is the file 'pi'.\n" },
      { "A/D/G/rho",     "This is the file 'rho'.\n" },
      { "A/D/G/tau",     "This is the file 'tau'.\n" },
      { "A/D/G/xi",      "This is the file 'xi'.\n" },
      { "A/D/I",         0 },
      { "A/D/I/delta",   "This is the file 'delta'.\n" },
      { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
    };
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
    SVN_ERR (validate_tree (revision_root, expected_entries, 21));
  }
  revisions[revision_count++] = after_rev;
  
  /* (3) E exists in both ANCESTOR and A, but refers to different
     nodes. */
  {
    /* (1) E exists in both ANCESTOR and B, but refers to different
       nodes, and not all nodes are directories.  Conflict. */

    /* ### kff todo: A/mu's contents will be exactly the same.
       If the fs ever starts optimizing this case, these tests may
       start to fail. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (svn_fs_delete (txn_root, "A/mu", pool));
    SVN_ERR (svn_fs_make_file (txn_root, "A/mu", pool));
    SVN_ERR (set_file_contents (txn_root, "A/mu", "This is the file 'mu'.\n"));
    SVN_ERR (test_commit_txn (&after_rev, txn, "/A/mu"));

    /* (1) E exists in both ANCESTOR and B, but refers to different
       revisions of the same node.  Conflict. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (set_file_contents (txn_root, "A/mu", "A change to file 'mu'.\n"));
    SVN_ERR (test_commit_txn (&after_rev, txn, "/A/mu"));

    /* (1) E exists in both ANCESTOR and B, and refers to the same
       node revision.  Replace E with A's node revision.  */
    {
      svn_string_t *old_mu_contents;
      SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
      SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
      SVN_ERR (get_file_contents (txn_root, "A/mu", &old_mu_contents));
      if ((! old_mu_contents) || (strcmp (old_mu_contents->data,
                                          "This is the file 'mu'.\n") != 0))
        {
          return svn_error_create
            (SVN_ERR_FS_GENERAL, 0, NULL, pool,
             "got wrong contents from an old revision tree");
        }
      SVN_ERR (svn_fs_make_file (txn_root, "A/sigma", pool));
      SVN_ERR (set_file_contents (txn_root, "A/sigma",  /* unrelated change */
                                  "This is the file 'sigma'.\n"));
      SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
      /*********************************************************************/
      /* REVISION 9 */
      /*********************************************************************/
      {
        tree_test_entry_t expected_entries[] = {
          /* path, contents (0 = dir) */
          { "theta",         "This is the file 'theta'.\n" },
          { "A",             0 },
          { "A/mu",          "A new file 'mu'.\n" },
          { "A/sigma",       "This is the file 'sigma'.\n" },
          { "A/B",           0 },
          { "A/B/lambda",    "This is the file 'lambda'.\n" },
          { "A/B/E",         0 },
          { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
          { "A/B/E/beta",    "This is the file 'beta'.\n" },
          { "A/B/F",         0 },
          { "A/C",           0 },
          { "A/C/kappa",     "This is the file 'kappa'.\n" },
          { "A/D",           0 },
          { "A/D/gamma",     "This is the file 'gamma'.\n" },
          { "A/D/G",         0 },
          { "A/D/G/pi",      "This is the file 'pi'.\n" },
          { "A/D/G/rho",     "This is the file 'rho'.\n" },
          { "A/D/G/tau",     "This is the file 'tau'.\n" },
          { "A/D/G/xi",      "This is the file 'xi'.\n" },
          { "A/D/I",         0 },
          { "A/D/I/delta",   "This is the file 'delta'.\n" },
          { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
        };
        SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
        SVN_ERR (validate_tree (revision_root, expected_entries, 22));
      }
      revisions[revision_count++] = after_rev;
    }
  }

  /* Preparation for upcoming tests. 
     We make a new head revision.  There are two changes in the new
     revision: A/B/lambda has been modified.  We will also use the
     recent addition of A/D/G/xi, treated as a modification to
     A/D/G. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[revision_count-1], pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (set_file_contents (txn_root, "A/B/lambda",
                              "Change to file 'lambda'.\n"));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
  /*********************************************************************/
  /* REVISION 10 */
  /*********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "theta",         "This is the file 'theta'.\n" },
      { "A",             0 },
      { "A/mu",          "A new file 'mu'.\n" },
      { "A/sigma",       "This is the file 'sigma'.\n" },
      { "A/B",           0 },
      { "A/B/lambda",    "Change to file 'lambda'.\n" },
      { "A/B/E",         0 },
      { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
      { "A/B/E/beta",    "This is the file 'beta'.\n" },
      { "A/B/F",         0 },
      { "A/C",           0 },
      { "A/C/kappa",     "This is the file 'kappa'.\n" },
      { "A/D",           0 },
      { "A/D/gamma",     "This is the file 'gamma'.\n" },
      { "A/D/G",         0 },
      { "A/D/G/pi",      "This is the file 'pi'.\n" },
      { "A/D/G/rho",     "This is the file 'rho'.\n" },
      { "A/D/G/tau",     "This is the file 'tau'.\n" },
      { "A/D/G/xi",      "This is the file 'xi'.\n" },
      { "A/D/I",         0 },
      { "A/D/I/delta",   "This is the file 'delta'.\n" },
      { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
    };
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
    SVN_ERR (validate_tree (revision_root, expected_entries, 22));
  }
  revisions[revision_count++] = after_rev;

  /* (2) E exists in both ANCESTOR and A, but refers to different 
     revisions of the same node. */
  {
    /* (1a) E exists in both ANCESTOR and B, but refers to different
       revisions of the same file node.  Conflict. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (set_file_contents (txn_root, "A/B/lambda",
                                "A different change to 'lambda'.\n"));
    SVN_ERR (test_commit_txn (&after_rev, txn, "/A/B/lambda"));

    /* (1b) E exists in both ANCESTOR and B, but refers to different
       revisions of the same directory node.  Merge A/E and B/E,
       recursively.  Succeed, because no conflict beneath E. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (svn_fs_make_file (txn_root, "A/D/G/nu", pool));
    SVN_ERR (set_file_contents (txn_root, "A/D/G/nu",
                                "This is the file 'nu'.\n"));
    SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
    /*********************************************************************/
    /* REVISION 11 */
    /*********************************************************************/
    {
      tree_test_entry_t expected_entries[] = {
        /* path, contents (0 = dir) */
        { "theta",         "This is the file 'theta'.\n" },
        { "A",             0 },
        { "A/mu",          "A new file 'mu'.\n" },
        { "A/sigma",       "This is the file 'sigma'.\n" },
        { "A/B",           0 },
        { "A/B/lambda",    "Change to file 'lambda'.\n" },
        { "A/B/E",         0 },
        { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
        { "A/B/E/beta",    "This is the file 'beta'.\n" },
        { "A/B/F",         0 },
        { "A/C",           0 },
        { "A/C/kappa",     "This is the file 'kappa'.\n" },
        { "A/D",           0 },
        { "A/D/gamma",     "This is the file 'gamma'.\n" },
        { "A/D/G",         0 },
        { "A/D/G/pi",      "This is the file 'pi'.\n" },
        { "A/D/G/rho",     "This is the file 'rho'.\n" },
        { "A/D/G/tau",     "This is the file 'tau'.\n" },
        { "A/D/G/xi",      "This is the file 'xi'.\n" },
        { "A/D/G/nu",      "This is the file 'nu'.\n" },
        { "A/D/I",         0 },
        { "A/D/I/delta",   "This is the file 'delta'.\n" },
        { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
      };
      SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
      SVN_ERR (validate_tree (revision_root, expected_entries, 23));
    }
    revisions[revision_count++] = after_rev;

    /* (1c) E exists in both ANCESTOR and B, but refers to different
       revisions of the same directory node.  Merge A/E and B/E,
       recursively.  Fail, because conflict beneath E. */
    SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
    SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
    SVN_ERR (svn_fs_make_file (txn_root, "A/D/G/xi", pool));
    SVN_ERR (set_file_contents (txn_root, "A/D/G/xi",
                                "This is a different file 'xi'.\n"));
    SVN_ERR (test_commit_txn (&after_rev, txn, "/A/D/G/xi"));

    /* (1) E exists in both ANCESTOR and B, and refers to the same node
       revision.  Replace E with A's node revision.  */
    {
      svn_string_t *old_lambda_ctnts;
      SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
      SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
      SVN_ERR (get_file_contents (txn_root, "A/B/lambda", &old_lambda_ctnts));
      if ((! old_lambda_ctnts)
          || (strcmp (old_lambda_ctnts->data,
                      "This is the file 'lambda'.\n") != 0))
        {
          return svn_error_create
            (SVN_ERR_FS_GENERAL, 0, NULL, pool,
             "got wrong contents from an old revision tree");
        }
      SVN_ERR (set_file_contents (txn_root, "A/D/G/rho",
                                  "This is an irrelevant change to 'rho'.\n"));
      SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
      /*********************************************************************/
      /* REVISION 12 */
      /*********************************************************************/
      {
        tree_test_entry_t expected_entries[] = {
          /* path, contents (0 = dir) */
          { "theta",         "This is the file 'theta'.\n" },
          { "A",             0 },
          { "A/mu",          "A new file 'mu'.\n" },
          { "A/sigma",       "This is the file 'sigma'.\n" },
          { "A/B",           0 },
          { "A/B/lambda",    "Change to file 'lambda'.\n" },
          { "A/B/E",         0 },
          { "A/B/E/alpha",   "This is the file 'alpha'.\n" },
          { "A/B/E/beta",    "This is the file 'beta'.\n" },
          { "A/B/F",         0 },
          { "A/C",           0 },
          { "A/C/kappa",     "This is the file 'kappa'.\n" },
          { "A/D",           0 },
          { "A/D/gamma",     "This is the file 'gamma'.\n" },
          { "A/D/G",         0 },
          { "A/D/G/pi",      "This is the file 'pi'.\n" },
          { "A/D/G/rho",     "This is an irrelevant change to 'rho'.\n" },
          { "A/D/G/tau",     "This is the file 'tau'.\n" },
          { "A/D/G/xi",      "This is the file 'xi'.\n" },
          { "A/D/G/nu",      "This is the file 'nu'.\n"},
          { "A/D/I",         0 },
          { "A/D/I/delta",   "This is the file 'delta'.\n" },
          { "A/D/I/epsilon", "This is the file 'epsilon'.\n" }
        };
        SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
        SVN_ERR (validate_tree (revision_root, expected_entries, 23));
      }
      revisions[revision_count++] = after_rev;
    }
  }  

  /* (1) E exists in both ANCESTOR and A, and refers to the same node
     revision. */
  {
    /* (1) E exists in both ANCESTOR and B, and refers to the same
       node revision.  Nothing has happened to ANCESTOR/E, so no
       change is necessary. */

    /* This has now been tested about fifty-four trillion times.  We
       don't need to test it again here. */
  }

  /* E exists in ANCESTOR, but has been deleted from A.  E exists in
     both ANCESTOR and B but refers to different nodes.  Conflict.  */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_delete (txn_root, "iota", pool));
  SVN_ERR (svn_fs_make_file (txn_root, "iota", pool));
  SVN_ERR (set_file_contents (txn_root, "iota", "New contents for 'iota'.\n"));
  SVN_ERR (test_commit_txn (&after_rev, txn, "/iota"));

  /* E exists in ANCESTOR, but has been deleted from A.  E exists in
     both ANCESTOR and B but refers to different revisions of the same
     node.  Conflict.  */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, revisions[1], pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (set_file_contents (txn_root, "iota", "New contents for 'iota'.\n"));
  SVN_ERR (test_commit_txn (&after_rev, txn, "/iota"));

  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


static svn_error_t *
copy_test (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root, *revision_root;
  svn_revnum_t after_rev;

  *msg = "testing svn_fs_copy on file and directory";

  /* Prepare a filesystem. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-copy-test"));

  /* In first txn, create and commit the greek tree. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (greek_tree_under_root (txn_root));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
  SVN_ERR (svn_fs_close_txn (txn));

  /* In second txn, copy the file A/D/G/pi into the subtree A/D/H as
     pi2.  Change that files contents to state its new name. */
  SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
  SVN_ERR (svn_fs_begin_txn (&txn, fs, after_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_copy (revision_root, "A/D/G/pi", 
                        txn_root, "A/D/H/pi2",
                        pool));
  SVN_ERR (set_file_contents (txn_root, "A/D/H/pi2", 
                              "This is the file 'pi2'.\n"));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
  SVN_ERR (svn_fs_close_txn (txn));

  /* Then, as if that wasn't fun enough, copy the whole subtree A/D/H
     into the root directory as H2! */
  SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
  SVN_ERR (svn_fs_begin_txn (&txn, fs, after_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_copy (revision_root, "A/D/H", 
                        txn_root, "H2",
                        pool));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
  SVN_ERR (svn_fs_close_txn (txn));

  /* Let's live dangerously.  What happens if we copy a path into one
     of its own children.  Looping filesystem?  Cyclic ancestry?
     Another West Virginia family tree with no branches?  We certainly
     hope that's not the case. */
  SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
  SVN_ERR (svn_fs_begin_txn (&txn, fs, after_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_copy (revision_root, "A/B", 
                        txn_root, "A/B/E/B",
                        pool));
  SVN_ERR (test_commit_txn (&after_rev, txn, NULL));
  SVN_ERR (svn_fs_close_txn (txn));

  /* After all these changes, let's see if the filesystem looks as we
     would expect it to. */
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "iota",        "This is the file 'iota'.\n" },
      { "H2",          0 },
      { "H2/chi",      "This is the file 'chi'.\n" },
      { "H2/pi2",      "This is the file 'pi2'.\n" },
      { "H2/psi",      "This is the file 'psi'.\n" },
      { "H2/omega",    "This is the file 'omega'.\n" },
      { "A",           0 },
      { "A/mu",        "This is the file 'mu'.\n" },
      { "A/B",         0 },
      { "A/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E",       0 },
      { "A/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/B/E/B",         0 },
      { "A/B/E/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E/B/E",       0 },
      { "A/B/E/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/B/E/B/F",       0 },
      { "A/B/F",       0 },
      { "A/C",         0 },
      { "A/D",         0 },
      { "A/D/gamma",   "This is the file 'gamma'.\n" },
      { "A/D/G",       0 },
      { "A/D/G/pi",    "This is the file 'pi'.\n" },
      { "A/D/G/rho",   "This is the file 'rho'.\n" },
      { "A/D/G/tau",   "This is the file 'tau'.\n" },
      { "A/D/H",       0 },
      { "A/D/H/chi",   "This is the file 'chi'.\n" },
      { "A/D/H/pi2",   "This is the file 'pi2'.\n" },
      { "A/D/H/psi",   "This is the file 'psi'.\n" },
      { "A/D/H/omega", "This is the file 'omega'.\n" }
    };
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, after_rev, pool)); 
    SVN_ERR (validate_tree (revision_root, expected_entries, 32));
  }
  /* Close the filesystem. */
  SVN_ERR (svn_fs_close_fs (fs));
  return SVN_NO_ERROR;
}


/* This tests deleting of mutable nodes.  We build a tree in a
 * transaction, then try to delete various items in the tree.  We
 * never commit the tree, so every entry being deleted points to a 
 * mutable node. 
 * 
 * ### todo: this test was written before commits worked.  It might
 * now be worthwhile to combine it with delete().
 */
static svn_error_t *
delete_mutables (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  svn_error_t *err;

  *msg = "delete mutable nodes from directories";

  /* Prepare a txn to receive the greek tree. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-del-from-dir"));
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  
  /* Create the greek tree. */
  SVN_ERR (greek_tree_under_root (txn_root));

  /* Baby, it's time to test like you've never tested before.  We do
   * the following, in this order:
   *
   *    1. Delete a single file somewhere, succeed.
   *    2. Delete two files of three, then make sure the third remains.
   *    3. Try to delete that directory, get the right error.
   *    4. Delete the third and last file.
   *    5. Try again to delete the dir, succeed.
   *    6. Delete one of the natively empty dirs, succeed.
   *    7. Try to delete root, fail.
   *    8. Try to delete a dir whose only entries are also dirs, fail.
   *    9. Try to delete a top-level file, succeed.
   *
   * Specifically, that's:
   *
   *    1. Delete A/D/gamma.
   *    2. Delete A/D/G/pi, A/D/G/rho.
   *    3. Try to delete A/D/G, fail.
   *    4. Delete A/D/G/tau.
   *    5. Try again to delete A/D/G, succeed.
   *    6. Delete A/C.
   *    7. Try to delete /, fail.
   *    8. Try to delete A/D, fail.
   *    9. Try to delete iota, succeed.
   *
   * Before and after each deletion or attempted deletion, we probe
   * the affected directory, to make sure everything is as it should
   * be.
   */

  /* 1 */
  {
    svn_fs_id_t *gamma_id;
    SVN_ERR (svn_fs_node_id (&gamma_id, txn_root, "A/D/gamma", pool));

    SVN_ERR (check_entry_present (txn_root, "A/D", "gamma"));
    SVN_ERR (check_id_present (fs, gamma_id));

    SVN_ERR (svn_fs_delete (txn_root, "A/D/gamma", pool));

    SVN_ERR (check_entry_absent (txn_root, "A/D", "gamma"));
    SVN_ERR (check_id_absent (fs, gamma_id));
  }

  /* 2 */
  {
    svn_fs_id_t *pi_id, *rho_id, *tau_id;
    SVN_ERR (svn_fs_node_id (&pi_id, txn_root, "A/D/G/pi", pool));
    SVN_ERR (svn_fs_node_id (&rho_id, txn_root, "A/D/G/rho", pool));
    SVN_ERR (svn_fs_node_id (&tau_id, txn_root, "A/D/G/tau", pool));

    SVN_ERR (check_entry_present (txn_root, "A/D/G", "pi"));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "rho"));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "tau"));
    SVN_ERR (check_id_present (fs, pi_id));
    SVN_ERR (check_id_present (fs, rho_id));
    SVN_ERR (check_id_present (fs, tau_id));

    SVN_ERR (svn_fs_delete (txn_root, "A/D/G/pi", pool));

    SVN_ERR (check_entry_absent (txn_root, "A/D/G", "pi"));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "rho"));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "tau"));
    SVN_ERR (check_id_absent (fs, pi_id));
    SVN_ERR (check_id_present (fs, rho_id));
    SVN_ERR (check_id_present (fs, tau_id));

    SVN_ERR (svn_fs_delete (txn_root, "A/D/G/rho", pool));

    SVN_ERR (check_entry_absent (txn_root, "A/D/G", "pi"));
    SVN_ERR (check_entry_absent (txn_root, "A/D/G", "rho"));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "tau"));
    SVN_ERR (check_id_absent (fs, pi_id));
    SVN_ERR (check_id_absent (fs, rho_id));
    SVN_ERR (check_id_present (fs, tau_id));
  }

  /* 3 */
  {
    svn_fs_id_t *G_id;
    SVN_ERR (svn_fs_node_id (&G_id, txn_root, "A/D/G", pool));

    SVN_ERR (check_id_present (fs, G_id));
    err = svn_fs_delete (txn_root, "A/D/G", pool);            /* fail */

    if (err && (err->apr_err != SVN_ERR_FS_DIR_NOT_EMPTY))
      {
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "deleting non-empty directory got wrong error");
      }
    else if (! err)
      {
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "deleting non-empty directory failed to get error");
      }

    SVN_ERR (check_entry_present (txn_root, "A/D", "G"));
    SVN_ERR (check_id_present (fs, G_id));
  }

  /* 4 */
  {
    svn_fs_id_t *tau_id;
    SVN_ERR (svn_fs_node_id (&tau_id, txn_root, "A/D/G/tau", pool));

    SVN_ERR (check_entry_present (txn_root, "A/D/G", "tau"));
    SVN_ERR (check_id_present (fs, tau_id));

    SVN_ERR (svn_fs_delete (txn_root, "A/D/G/tau", pool));

    SVN_ERR (check_entry_absent (txn_root, "A/D/G", "tau"));
    SVN_ERR (check_id_absent (fs, tau_id));
  }

  /* 5 */
  {
    svn_fs_id_t *G_id;
    SVN_ERR (svn_fs_node_id (&G_id, txn_root, "A/D/G", pool));

    SVN_ERR (check_entry_present (txn_root, "A/D", "G"));
    SVN_ERR (check_id_present (fs, G_id));

    SVN_ERR (svn_fs_delete (txn_root, "A/D/G", pool));        /* succeed */

    SVN_ERR (check_entry_absent (txn_root, "A/D", "G"));
    SVN_ERR (check_id_absent (fs, G_id));
  }

  /* 6 */
  {
    svn_fs_id_t *C_id;
    SVN_ERR (svn_fs_node_id (&C_id, txn_root, "A/C", pool));

    SVN_ERR (check_entry_present (txn_root, "A", "C"));
    SVN_ERR (check_id_present (fs, C_id));

    SVN_ERR (svn_fs_delete (txn_root, "A/C", pool));

    SVN_ERR (check_entry_absent (txn_root, "A", "C"));
    SVN_ERR (check_id_absent (fs, C_id));
  }

  /* 7 */
  {
    svn_fs_id_t *root_id;
    SVN_ERR (svn_fs_node_id (&root_id, txn_root, "", pool));

    err = svn_fs_delete (txn_root, "", pool);

    if (err && (err->apr_err != SVN_ERR_FS_ROOT_DIR))
      {
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "deleting root directory got wrong error");
      }
    else if (! err)
      {
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "deleting root directory failed to get error");
      }

    SVN_ERR (check_id_present (fs, root_id));
  }

  /* 8 */
  {
    svn_fs_id_t *D_id;
    SVN_ERR (svn_fs_node_id (&D_id, txn_root, "A/D", pool));

    err = svn_fs_delete (txn_root, "A/D", pool);

    if (err && (err->apr_err != SVN_ERR_FS_DIR_NOT_EMPTY))
      {
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "deleting non-empty directory got wrong error");
      }
    else if (! err)
      {
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "deleting non-empty directory failed to get error");
      }

    SVN_ERR (check_entry_present (txn_root, "A", "D"));
    SVN_ERR (check_id_present (fs, D_id));
  }
  
  /* 9 */
  {
    svn_fs_id_t *iota_id;
    SVN_ERR (svn_fs_node_id (&iota_id, txn_root, "iota", pool));

    SVN_ERR (check_entry_present (txn_root, "", "iota"));
    SVN_ERR (check_id_present (fs, iota_id));

    SVN_ERR (svn_fs_delete (txn_root, "iota", pool));

    SVN_ERR (check_entry_absent (txn_root, "", "iota"));
    SVN_ERR (check_id_absent (fs, iota_id));
  }

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


/* This tests deleting in general.
 * 
 * ### todo: this test was written after (and independently of)
 * delete_mutables().  It might be worthwhile to combine them.
 */
static svn_error_t *
delete (const char **msg)
{
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root;
  svn_revnum_t new_rev;
  svn_error_t *err;

  *msg = "delete nodes tree";

  /* This function tests 5 cases:
   *
   * 1. Delete mutable file.
   * 2. Delete mutable directory.
   * 3. Delete mutable directory with immutable nodes.
   * 4. Delete immutable file.
   * 5. Delete immutable directory.
   */

  /* Prepare a txn to receive the greek tree. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-del-tree"));
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree. */
  SVN_ERR (greek_tree_under_root (txn_root));

  /* 1. Delete mutable file. */
  {
    svn_fs_id_t *iota_id, *gamma_id;
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "A",           0 },
      { "A/mu",        "This is the file 'mu'.\n" },
      { "A/B",         0 },
      { "A/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E",       0 },
      { "A/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/C",         0 },
      { "A/B/F",       0 },
      { "A/D",         0 },
      { "A/D/G",       0 },
      { "A/D/G/pi",    "This is the file 'pi'.\n" },
      { "A/D/G/rho",   "This is the file 'rho'.\n" },
      { "A/D/G/tau",   "This is the file 'tau'.\n" },
      { "A/D/H",       0 },
      { "A/D/H/chi",   "This is the file 'chi'.\n" },
      { "A/D/H/psi",   "This is the file 'psi'.\n" },
      { "A/D/H/omega", "This is the file 'omega'.\n" }
    };

    /* Check nodes revision ID is gone.  */
    SVN_ERR (svn_fs_node_id (&iota_id, txn_root, "iota", pool));
    SVN_ERR (svn_fs_node_id (&gamma_id, txn_root, "A/D/gamma", pool));

    SVN_ERR (check_entry_present (txn_root, "", "iota"));
    SVN_ERR (check_id_present (fs, iota_id));
    SVN_ERR (check_id_present (fs, gamma_id));

    /* Try deleting a mutable file with plain delete. */
    SVN_ERR (svn_fs_delete (txn_root, "iota", pool));
    SVN_ERR (check_entry_absent (txn_root, "", "iota"));
    SVN_ERR (check_id_absent (fs, iota_id));

    /* Try deleting a mutable file with delete_tree. */
    SVN_ERR (svn_fs_delete_tree (txn_root, "A/D/gamma", pool));
    SVN_ERR (check_entry_absent (txn_root, "A/D", "gamma"));
    SVN_ERR (check_id_absent (fs, gamma_id));

    /* Validate the tree.  */
    SVN_ERR (validate_tree (txn_root, expected_entries, 18));
  }
  /* Abort transaction.  */
  SVN_ERR (svn_fs_abort_txn (txn));

  /* 2. Delete mutable directory. */

  /* Prepare a txn to receive the greek tree. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree. */
  SVN_ERR (greek_tree_under_root (txn_root));

  {
    svn_fs_id_t *A_id, *mu_id, *B_id, *lambda_id, *E_id, *alpha_id,
      *beta_id, *F_id, *C_id, *D_id, *gamma_id, *H_id, *chi_id,
      *psi_id, *omega_id, *G_id, *pi_id, *rho_id, *tau_id;

    /* Check nodes revision ID is gone.  */
    SVN_ERR (svn_fs_node_id (&A_id, txn_root, "/A", pool));
    SVN_ERR (check_entry_present (txn_root, "", "A"));
    SVN_ERR (svn_fs_node_id (&mu_id, txn_root, "/A/mu", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "mu"));
    SVN_ERR (svn_fs_node_id (&B_id, txn_root, "/A/B", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "B"));
    SVN_ERR (svn_fs_node_id (&lambda_id, txn_root, "/A/B/lambda", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "lambda"));
    SVN_ERR (svn_fs_node_id (&E_id, txn_root, "/A/B/E", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "E"));
    SVN_ERR (svn_fs_node_id (&alpha_id, txn_root, "/A/B/E/alpha", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B/E", "alpha"));
    SVN_ERR (svn_fs_node_id (&beta_id, txn_root, "/A/B/E/beta", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B/E", "beta"));
    SVN_ERR (svn_fs_node_id (&F_id, txn_root, "/A/B/F", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "F"));
    SVN_ERR (svn_fs_node_id (&C_id, txn_root, "/A/C", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "C"));
    SVN_ERR (svn_fs_node_id (&D_id, txn_root, "/A/D", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "D"));
    SVN_ERR (svn_fs_node_id (&gamma_id, txn_root, "/A/D/gamma", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "gamma"));
    SVN_ERR (svn_fs_node_id (&H_id, txn_root, "/A/D/H", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "H"));
    SVN_ERR (svn_fs_node_id (&chi_id, txn_root, "/A/D/H/chi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "chi"));
    SVN_ERR (svn_fs_node_id (&psi_id, txn_root, "/A/D/H/psi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "psi"));
    SVN_ERR (svn_fs_node_id (&omega_id, txn_root, "/A/D/H/omega", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "omega"));
    SVN_ERR (svn_fs_node_id (&G_id, txn_root, "/A/D/G", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "G"));
    SVN_ERR (svn_fs_node_id (&pi_id, txn_root, "/A/D/G/pi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "pi"));
    SVN_ERR (svn_fs_node_id (&rho_id, txn_root, "/A/D/G/rho", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "rho"));
    SVN_ERR (svn_fs_node_id (&tau_id, txn_root, "/A/D/G/tau", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "tau"));

    /* Try deleting a mutable empty dir with plain delete. */
    SVN_ERR (svn_fs_delete (txn_root, "A/C", pool));
    SVN_ERR (check_entry_absent (txn_root, "A", "C"));
    SVN_ERR (check_id_absent (fs, C_id));

    /* Try deleting a mutable empty dir with delete_tree. */
    SVN_ERR (svn_fs_delete_tree (txn_root, "A/B/F", pool));
    SVN_ERR (check_entry_absent (txn_root, "A/B", "F"));
    SVN_ERR (check_id_absent (fs, F_id));

    /* Try an unsuccessful delete of a non-empty dir. */
    err = svn_fs_delete (txn_root, "A", pool);
    if (err && (err->apr_err != SVN_ERR_FS_DIR_NOT_EMPTY))
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "delete failed as expected, but for wrong reason");
      }
    else if (! err)
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "delete succeeded when expected to fail");
      }

    /* Try a successful delete of a non-empty dir. */
    SVN_ERR (svn_fs_delete_tree (txn_root, "A", pool));

    SVN_ERR (check_entry_absent (txn_root, "", "A"));
    SVN_ERR (check_id_absent (fs, A_id));
    SVN_ERR (check_id_absent (fs, mu_id));
    SVN_ERR (check_id_absent (fs, B_id));
    SVN_ERR (check_id_absent (fs, lambda_id));
    SVN_ERR (check_id_absent (fs, E_id));
    SVN_ERR (check_id_absent (fs, alpha_id));
    SVN_ERR (check_id_absent (fs, beta_id));
    SVN_ERR (check_id_absent (fs, D_id));
    SVN_ERR (check_id_absent (fs, gamma_id));
    SVN_ERR (check_id_absent (fs, H_id));
    SVN_ERR (check_id_absent (fs, chi_id));
    SVN_ERR (check_id_absent (fs, psi_id));
    SVN_ERR (check_id_absent (fs, omega_id));
    SVN_ERR (check_id_absent (fs, G_id));
    SVN_ERR (check_id_absent (fs, pi_id));
    SVN_ERR (check_id_absent (fs, rho_id));
    SVN_ERR (check_id_absent (fs, tau_id));

    /* Validate the tree.  */
    {
      tree_test_entry_t expected_entries[] = {
        /* path, contents (0 = dir) */
        { "iota",        "This is the file 'iota'.\n" } };
      SVN_ERR (validate_tree (txn_root, expected_entries, 1));
    }
  }

  /* Abort transaction.  */
  SVN_ERR (svn_fs_abort_txn (txn));

  /* 3. Delete mutable directory with immutable nodes. */

  /* Prepare a txn to receive the greek tree. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create the greek tree. */
  SVN_ERR (greek_tree_under_root (txn_root));

  /* Commit the greek tree. */
  SVN_ERR (svn_fs_commit_txn (NULL, &new_rev, txn));
  SVN_ERR (svn_fs_close_txn (txn));

  /* Create new transaction. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, new_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  {
    svn_fs_id_t *A_id, *mu_id, *B_id, *lambda_id, *E_id, *alpha_id,
      *beta_id, *F_id, *C_id, *D_id, *gamma_id, *H_id, *chi_id,
      *psi_id, *omega_id, *G_id, *pi_id, *rho_id, *tau_id, *sigma_id;

    /* Create A/D/G/sigma.  This makes all component of A/D/G
       mutable.  */
    SVN_ERR (svn_fs_make_file (txn_root, "A/D/G/sigma", pool));
    SVN_ERR (set_file_contents (txn_root, "A/D/G/sigma",
                                "This is another file 'sigma'.\n"));

    /* Check mutable nodes revision ID is removed and immutable ones
       still exist.  */
    SVN_ERR (svn_fs_node_id (&A_id, txn_root, "/A", pool));
    SVN_ERR (check_entry_present (txn_root, "", "A"));
    SVN_ERR (svn_fs_node_id (&mu_id, txn_root, "/A/mu", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "mu"));
    SVN_ERR (svn_fs_node_id (&B_id, txn_root, "/A/B", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "B"));
    SVN_ERR (svn_fs_node_id (&lambda_id, txn_root, "/A/B/lambda", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "lambda"));
    SVN_ERR (svn_fs_node_id (&E_id, txn_root, "/A/B/E", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "E"));
    SVN_ERR (svn_fs_node_id (&alpha_id, txn_root, "/A/B/E/alpha", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B/E", "alpha"));
    SVN_ERR (svn_fs_node_id (&beta_id, txn_root, "/A/B/E/beta", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B/E", "beta"));
    SVN_ERR (svn_fs_node_id (&F_id, txn_root, "/A/B/F", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "F"));
    SVN_ERR (svn_fs_node_id (&C_id, txn_root, "/A/C", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "C"));
    SVN_ERR (svn_fs_node_id (&D_id, txn_root, "/A/D", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "D"));
    SVN_ERR (svn_fs_node_id (&gamma_id, txn_root, "/A/D/gamma", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "gamma"));
    SVN_ERR (svn_fs_node_id (&H_id, txn_root, "/A/D/H", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "H"));
    SVN_ERR (svn_fs_node_id (&chi_id, txn_root, "/A/D/H/chi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "chi"));
    SVN_ERR (svn_fs_node_id (&psi_id, txn_root, "/A/D/H/psi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "psi"));
    SVN_ERR (svn_fs_node_id (&omega_id, txn_root, "/A/D/H/omega", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "omega"));
    SVN_ERR (svn_fs_node_id (&G_id, txn_root, "/A/D/G", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "G"));
    SVN_ERR (svn_fs_node_id (&pi_id, txn_root, "/A/D/G/pi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "pi"));
    SVN_ERR (svn_fs_node_id (&rho_id, txn_root, "/A/D/G/rho", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "rho"));
    SVN_ERR (svn_fs_node_id (&tau_id, txn_root, "/A/D/G/tau", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "tau"));
    SVN_ERR (svn_fs_node_id (&sigma_id, txn_root, "/A/D/G/sigma", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "sigma"));

    /* First try an unsuccessful delete. */
    err = svn_fs_delete (txn_root, "A", pool);
    if (err && (err->apr_err != SVN_ERR_FS_DIR_NOT_EMPTY))
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "delete failed as expected, but for wrong reason");
      }
    else if (! err)
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "delete succeeded when expected to fail");
      }

    /* Then try a successful delete. */
    SVN_ERR (svn_fs_delete_tree (txn_root, "A", pool));

    SVN_ERR (check_entry_absent (txn_root, "", "A"));
    SVN_ERR (check_id_absent (fs, A_id));
    SVN_ERR (check_id_present (fs, mu_id));
    SVN_ERR (check_id_present (fs, B_id));
    SVN_ERR (check_id_present (fs, lambda_id));
    SVN_ERR (check_id_present (fs, E_id));
    SVN_ERR (check_id_present (fs, alpha_id));
    SVN_ERR (check_id_present (fs, beta_id));
    SVN_ERR (check_id_present (fs, F_id));
    SVN_ERR (check_id_present (fs, C_id));
    SVN_ERR (check_id_absent (fs, D_id));
    SVN_ERR (check_id_present (fs, gamma_id));
    SVN_ERR (check_id_present (fs, H_id));
    SVN_ERR (check_id_present (fs, chi_id));
    SVN_ERR (check_id_present (fs, psi_id));
    SVN_ERR (check_id_present (fs, omega_id));
    SVN_ERR (check_id_absent (fs, G_id));
    SVN_ERR (check_id_present (fs, pi_id));
    SVN_ERR (check_id_present (fs, rho_id));
    SVN_ERR (check_id_present (fs, tau_id));
    SVN_ERR (check_id_absent (fs, sigma_id));

    /* Validate the tree.  */
    {
      tree_test_entry_t expected_entries[] = {
        /* path, contents (0 = dir) */
        { "iota",        "This is the file 'iota'.\n" }
      };
      
      SVN_ERR (validate_tree (txn_root, expected_entries, 1));
    }
  }

  /* Abort transaction.  */
  SVN_ERR (svn_fs_abort_txn (txn));

  /* 4. Delete immutable file. */

  /* Create new transaction. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, new_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  {
    svn_fs_id_t *iota_id, *gamma_id;

    /* Check nodes revision ID is present.  */
    SVN_ERR (svn_fs_node_id (&iota_id, txn_root, "iota", pool));
    SVN_ERR (svn_fs_node_id (&gamma_id, txn_root, "A/D/gamma", pool));
    SVN_ERR (check_entry_present (txn_root, "", "iota"));
    SVN_ERR (check_entry_present (txn_root, "A/D", "gamma"));
    SVN_ERR (check_id_present (fs, iota_id));
    SVN_ERR (check_id_present (fs, gamma_id));

    /* Try it once with plain delete(). */
    SVN_ERR (svn_fs_delete (txn_root, "iota", pool));
    SVN_ERR (check_entry_absent (txn_root, "", "iota"));
    SVN_ERR (check_id_present (fs, iota_id));

    /* Try it once with delete_tree(). */
    SVN_ERR (svn_fs_delete_tree (txn_root, "A/D/gamma", pool));
    SVN_ERR (check_entry_absent (txn_root, "A/D", "iota"));
    SVN_ERR (check_id_present (fs, gamma_id));

    /* Validate the tree.  */
    {
      tree_test_entry_t expected_entries[] = {
        /* path, contents (0 = dir) */
        { "A",           0 },
        { "A/mu",        "This is the file 'mu'.\n" },
        { "A/B",         0 },
        { "A/B/lambda",  "This is the file 'lambda'.\n" },
        { "A/B/E",       0 },
        { "A/B/E/alpha", "This is the file 'alpha'.\n" },
        { "A/B/E/beta",  "This is the file 'beta'.\n" },
        { "A/B/F",       0 },
        { "A/C",         0 },
        { "A/D",         0 },
        { "A/D/G",       0 },
        { "A/D/G/pi",    "This is the file 'pi'.\n" },
        { "A/D/G/rho",   "This is the file 'rho'.\n" },
        { "A/D/G/tau",   "This is the file 'tau'.\n" },
        { "A/D/H",       0 },
        { "A/D/H/chi",   "This is the file 'chi'.\n" },
        { "A/D/H/psi",   "This is the file 'psi'.\n" },
        { "A/D/H/omega", "This is the file 'omega'.\n" }
      };
      SVN_ERR (validate_tree (txn_root, expected_entries, 18));
    }
  }

  /* Abort transaction.  */
  SVN_ERR (svn_fs_abort_txn (txn));

  /* 5. Delete immutable directory. */

  /* Create new transaction. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, new_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  {
    svn_fs_id_t *A_id, *mu_id, *B_id, *lambda_id, *E_id, *alpha_id,
      *beta_id, *F_id, *C_id, *D_id, *gamma_id, *H_id, *chi_id,
      *psi_id, *omega_id, *G_id, *pi_id, *rho_id, *tau_id;

    /* Check nodes revision ID is present.  */
    SVN_ERR (svn_fs_node_id (&A_id, txn_root, "/A", pool));
    SVN_ERR (check_entry_present (txn_root, "", "A"));
    SVN_ERR (svn_fs_node_id (&mu_id, txn_root, "/A/mu", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "mu"));
    SVN_ERR (svn_fs_node_id (&B_id, txn_root, "/A/B", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "B"));
    SVN_ERR (svn_fs_node_id (&lambda_id, txn_root, "/A/B/lambda", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "lambda"));
    SVN_ERR (svn_fs_node_id (&E_id, txn_root, "/A/B/E", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "E"));
    SVN_ERR (svn_fs_node_id (&alpha_id, txn_root, "/A/B/E/alpha", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B/E", "alpha"));
    SVN_ERR (svn_fs_node_id (&beta_id, txn_root, "/A/B/E/beta", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B/E", "beta"));
    SVN_ERR (svn_fs_node_id (&F_id, txn_root, "/A/B/F", pool));
    SVN_ERR (check_entry_present (txn_root, "A/B", "F"));
    SVN_ERR (svn_fs_node_id (&C_id, txn_root, "/A/C", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "C"));
    SVN_ERR (svn_fs_node_id (&D_id, txn_root, "/A/D", pool));
    SVN_ERR (check_entry_present (txn_root, "A", "D"));
    SVN_ERR (svn_fs_node_id (&gamma_id, txn_root, "/A/D/gamma", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "gamma"));
    SVN_ERR (svn_fs_node_id (&H_id, txn_root, "/A/D/H", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "H"));
    SVN_ERR (svn_fs_node_id (&chi_id, txn_root, "/A/D/H/chi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "chi"));
    SVN_ERR (svn_fs_node_id (&psi_id, txn_root, "/A/D/H/psi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "psi"));
    SVN_ERR (svn_fs_node_id (&omega_id, txn_root, "/A/D/H/omega", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/H", "omega"));
    SVN_ERR (svn_fs_node_id (&G_id, txn_root, "/A/D/G", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D", "G"));
    SVN_ERR (svn_fs_node_id (&pi_id, txn_root, "/A/D/G/pi", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "pi"));
    SVN_ERR (svn_fs_node_id (&rho_id, txn_root, "/A/D/G/rho", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "rho"));
    SVN_ERR (svn_fs_node_id (&tau_id, txn_root, "/A/D/G/tau", pool));
    SVN_ERR (check_entry_present (txn_root, "A/D/G", "tau"));

    /* First try an unsuccessful delete. */
    err = svn_fs_delete (txn_root, "A", pool);
    if (err && (err->apr_err != SVN_ERR_FS_DIR_NOT_EMPTY))
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "delete failed as expected, but for wrong reason");
      }
    else if (! err)
      {
        return svn_error_create
          (SVN_ERR_FS_GENERAL, 0, NULL, pool,
           "delete succeeded when expected to fail");
      }

    /* Then try a successful delete. */
    SVN_ERR (svn_fs_delete_tree (txn_root, "A", pool));

    SVN_ERR (check_entry_absent (txn_root, "", "A"));
    SVN_ERR (check_id_present (fs, A_id));
    SVN_ERR (check_id_present (fs, mu_id));
    SVN_ERR (check_id_present (fs, B_id));
    SVN_ERR (check_id_present (fs, lambda_id));
    SVN_ERR (check_id_present (fs, E_id));
    SVN_ERR (check_id_present (fs, alpha_id));
    SVN_ERR (check_id_present (fs, beta_id));
    SVN_ERR (check_id_present (fs, F_id));
    SVN_ERR (check_id_present (fs, C_id));
    SVN_ERR (check_id_present (fs, D_id));
    SVN_ERR (check_id_present (fs, gamma_id));
    SVN_ERR (check_id_present (fs, H_id));
    SVN_ERR (check_id_present (fs, chi_id));
    SVN_ERR (check_id_present (fs, psi_id));
    SVN_ERR (check_id_present (fs, omega_id));
    SVN_ERR (check_id_present (fs, G_id));
    SVN_ERR (check_id_present (fs, pi_id));
    SVN_ERR (check_id_present (fs, rho_id));
    SVN_ERR (check_id_present (fs, tau_id));

    /* Validate the tree.  */
    {
      tree_test_entry_t expected_entries[] = {
        /* path, contents (0 = dir) */
        { "iota",        "This is the file 'iota'.\n" }
      };
      SVN_ERR (validate_tree (txn_root, expected_entries, 1));
    }
  }

  /* Close the transaction and fs. */
  SVN_ERR (svn_fs_close_txn (txn));
  SVN_ERR (svn_fs_close_fs (fs));

  return SVN_NO_ERROR;
}


static svn_error_t *
dir_deltas (const char **msg)
{ 
  svn_fs_t *fs;
  svn_fs_txn_t *txn;
  svn_fs_root_t *txn_root, *revision_root;
  svn_revnum_t youngest_rev;
  void *edit_baton;
  const svn_delta_edit_fns_t *editor;
  tree_test_t expected_trees[8];
  int revision_count = 0;
  int i, j;

  *msg = "test svn_fs_dir_delta";

  /* The Test Plan
     
     The filesystem function svn_fs_dir_delta exists to drive an
     editor in such a way that given a source tree S and a target tree
     T, that editor manipulation will transform S into T, insomuch as
     directories and files, and their contents and properties, go.
     The general notion of the test plan will be to create pairs of
     trees (S, T), and an editor that edits a copy of tree S, run them
     through svn_fs_dir_delta, and then verify that the edited copy of
     S is identical to T when it is all said and done.
  */

  /* Create a filesystem and repository. */
  SVN_ERR (create_fs_and_repos (&fs, "test-repo-dir-deltas"));
  expected_trees[revision_count].num_entries = 0;
  expected_trees[revision_count++].entries = 0;

  /* Prepare a txn to receive the greek tree. */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, 0, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

  /* Create and commit the greek tree. */
  SVN_ERR (greek_tree_under_root (txn_root));
  SVN_ERR (svn_fs_commit_txn (NULL, &youngest_rev, txn));
  SVN_ERR (svn_fs_close_txn (txn));

  /***********************************************************************/
  /* REVISION 1 */
  /***********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "iota",        "This is the file 'iota'.\n" },
      { "A",           0 },
      { "A/mu",        "This is the file 'mu'.\n" },
      { "A/B",         0 },
      { "A/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E",       0 },
      { "A/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/B/F",       0 },
      { "A/C",         0 },
      { "A/D",         0 },
      { "A/D/gamma",   "This is the file 'gamma'.\n" },
      { "A/D/G",       0 },
      { "A/D/G/pi",    "This is the file 'pi'.\n" },
      { "A/D/G/rho",   "This is the file 'rho'.\n" },
      { "A/D/G/tau",   "This is the file 'tau'.\n" },
      { "A/D/H",       0 },
      { "A/D/H/chi",   "This is the file 'chi'.\n" },
      { "A/D/H/psi",   "This is the file 'psi'.\n" },
      { "A/D/H/omega", "This is the file 'omega'.\n" }
    };
    expected_trees[revision_count].entries = expected_entries;
    expected_trees[revision_count].num_entries = 20;
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, youngest_rev, pool)); 
    SVN_ERR (validate_tree (revision_root,
                            expected_trees[revision_count].entries,
                            expected_trees[revision_count].num_entries));
    revision_count++;
  }

  /* Make a new txn based on the youngest revision, make some changes,
     and commit those changes (which makes a new youngest
     revision). */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, youngest_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  {
    txn_script_command_t script_entries[] = {
      { '+', "A/delta",     "This is the file 'delta'.\n" },
      { '+', "A/epsilon",   "This is the file 'epsilon'.\n" },
      { '+', "A/B/Z",       0 },
      { '+', "A/B/Z/zeta",  "This is the file 'zeta'.\n" },
      { '-', "A/C",         0 },
      { '-', "A/mu"         "" },
      { '-', "A/D/G/tau",   "" },
      { '-', "A/D/H/omega", "" },
      { '>', "iota",        "Changed file 'iota'.\n" },
      { '>', "A/D/G/rho",   "Changed file 'rho'.\n" }
    };
    SVN_ERR (txn_script_exec (txn_root, script_entries, 10));
  }
  SVN_ERR (svn_fs_commit_txn (NULL, &youngest_rev, txn));
  SVN_ERR (svn_fs_close_txn (txn));

  /***********************************************************************/
  /* REVISION 2 */
  /***********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "iota",        "Changed file 'iota'.\n" },
      { "A",           0 },
      { "A/delta",     "This is the file 'delta'.\n" },
      { "A/epsilon",   "This is the file 'epsilon'.\n" },
      { "A/B",         0 },
      { "A/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E",       0 },
      { "A/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/B/F",       0 },
      { "A/B/Z",       0 },
      { "A/B/Z/zeta",  "This is the file 'zeta'.\n" },
      { "A/D",         0 },
      { "A/D/gamma",   "This is the file 'gamma'.\n" },
      { "A/D/G",       0 },
      { "A/D/G/pi",    "This is the file 'pi'.\n" },
      { "A/D/G/rho",   "Changed file 'rho'.\n" },
      { "A/D/H",       0 },
      { "A/D/H/chi",   "This is the file 'chi'.\n" },
      { "A/D/H/psi",   "This is the file 'psi'.\n" }
    };
    expected_trees[revision_count].entries = expected_entries;
    expected_trees[revision_count].num_entries = 20;
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, youngest_rev, pool)); 
    SVN_ERR (validate_tree (revision_root,
                            expected_trees[revision_count].entries,
                            expected_trees[revision_count].num_entries));
    revision_count++;
  } 

  /* Make a new txn based on the youngest revision, make some changes,
     and commit those changes (which makes a new youngest
     revision). */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, youngest_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  {
    txn_script_command_t script_entries[] = {
      { '+', "A/mu",        "Re-added file 'mu'.\n" },
      { '+', "A/D/H/omega", 0 }, /* re-add omega as directory! */
      { '-', "iota",        "" },
      { '>', "A/delta",     "This is the file 'delta'.\nLine 2.\n" }
    };
    SVN_ERR (txn_script_exec (txn_root, script_entries, 4));
  }
  SVN_ERR (svn_fs_commit_txn (NULL, &youngest_rev, txn));
  SVN_ERR (svn_fs_close_txn (txn));

  /***********************************************************************/
  /* REVISION 3 */
  /***********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "A",           0 },
      { "A/delta",     "This is the file 'delta'.\nLine 2.\n" },
      { "A/epsilon",   "This is the file 'epsilon'.\n" },
      { "A/mu",        "Re-added file 'mu'.\n" },
      { "A/B",         0 },
      { "A/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E",       0 },
      { "A/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/B/F",       0 },
      { "A/B/Z",       0 },
      { "A/B/Z/zeta",  "This is the file 'zeta'.\n" },
      { "A/D",         0 },
      { "A/D/gamma",   "This is the file 'gamma'.\n" },
      { "A/D/G",       0 },
      { "A/D/G/pi",    "This is the file 'pi'.\n" },
      { "A/D/G/rho",   "Changed file 'rho'.\n" },
      { "A/D/H",       0 },
      { "A/D/H/chi",   "This is the file 'chi'.\n" },
      { "A/D/H/psi",   "This is the file 'psi'.\n" },
      { "A/D/H/omega", 0 }
    };
    expected_trees[revision_count].entries = expected_entries;
    expected_trees[revision_count].num_entries = 21;
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, youngest_rev, pool)); 
    SVN_ERR (validate_tree (revision_root,
                            expected_trees[revision_count].entries,
                            expected_trees[revision_count].num_entries));
    revision_count++;
  }

  /* Make a new txn based on the youngest revision, make some changes,
     and commit those changes (which makes a new youngest
     revision). */
  SVN_ERR (svn_fs_begin_txn (&txn, fs, youngest_rev, pool));
  SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));
  SVN_ERR (svn_fs_copy (revision_root, "A/D/G",
                        txn_root, "A/D/G2",
                        pool));
  SVN_ERR (svn_fs_copy (revision_root, "A/epsilon",
                        txn_root, "A/B/epsilon",
                        pool));
  SVN_ERR (svn_fs_commit_txn (NULL, &youngest_rev, txn));
  SVN_ERR (svn_fs_close_txn (txn));

  /***********************************************************************/
  /* REVISION 4 */
  /***********************************************************************/
  {
    tree_test_entry_t expected_entries[] = {
      /* path, contents (0 = dir) */
      { "A",           0 },
      { "A/delta",     "This is the file 'delta'.\nLine 2.\n" },
      { "A/epsilon",   "This is the file 'epsilon'.\n" },
      { "A/mu",        "Re-added file 'mu'.\n" },
      { "A/B",         0 },
      { "A/B/epsilon", "This is the file 'epsilon'.\n" },
      { "A/B/lambda",  "This is the file 'lambda'.\n" },
      { "A/B/E",       0 },
      { "A/B/E/alpha", "This is the file 'alpha'.\n" },
      { "A/B/E/beta",  "This is the file 'beta'.\n" },
      { "A/B/F",       0 },
      { "A/B/Z",       0 },
      { "A/B/Z/zeta",  "This is the file 'zeta'.\n" },
      { "A/D",         0 },
      { "A/D/gamma",   "This is the file 'gamma'.\n" },
      { "A/D/G",       0 },
      { "A/D/G/pi",    "This is the file 'pi'.\n" },
      { "A/D/G/rho",   "Changed file 'rho'.\n" },
      { "A/D/G2",      0 },
      { "A/D/G2/pi",   "This is the file 'pi'.\n" },
      { "A/D/G2/rho",  "Changed file 'rho'.\n" },
      { "A/D/H",       0 },
      { "A/D/H/chi",   "This is the file 'chi'.\n" },
      { "A/D/H/psi",   "This is the file 'psi'.\n" },
      { "A/D/H/omega", 0 }
    };
    expected_trees[revision_count].entries = expected_entries;
    expected_trees[revision_count].num_entries = 25;
    SVN_ERR (svn_fs_revision_root (&revision_root, fs, youngest_rev, pool)); 
    SVN_ERR (validate_tree (revision_root,
                            expected_trees[revision_count].entries,
                            expected_trees[revision_count].num_entries));
    revision_count++;
  }

  /* THE BIG IDEA: Now that we have a collection of revisions, let's
     first make sure that given any two revisions, we can get the
     right delta between them.  We'll do this by selecting our two
     revisions, R1 and R2, basing a transaction off R1, deltafying the
     txn with respect to R2, and then making sure our final txn looks
     exactly like R2.  This should work regardless of the
     chronological order in which R1 and R2 were created.  */

  for (i = 0; i < revision_count; i++)
    {
      for (j = 0; j < revision_count; j++)
        {
          svn_revnum_t *revision;
          apr_hash_t *rev_diffs;

          /* Initialize our source revisions hash. */
          rev_diffs = apr_hash_make (pool);
          revision = apr_pcalloc (pool, sizeof (svn_revnum_t));
          *revision = i;
          apr_hash_set (rev_diffs, "", APR_HASH_KEY_STRING, revision);

          /* Prepare a txn that will receive the changes from
             svn_fs_dir_delta */
          SVN_ERR (svn_fs_begin_txn (&txn, fs, i, pool));
          SVN_ERR (svn_fs_txn_root (&txn_root, txn, pool));

          /* Get the editor that will be modifying our transaction. */
          SVN_ERR (dir_delta_get_editor (&editor,
                                         &edit_baton,
                                         fs,
                                         txn_root,
                                         svn_string_create ("", pool),
                                         pool));
    
          /* Here's the kicker...do the directory delta. */
          SVN_ERR (svn_fs_revision_root (&revision_root, fs, j, pool)); 
          SVN_ERR (svn_fs_dir_delta (txn_root,
                                     "",
                                     rev_diffs,
                                     revision_root,
                                     "",
                                     editor,
                                     edit_baton,
                                     pool));

          /* Hopefully at this point our transaction has been modified
             to look exactly like our latest revision.  We'll check
             that. */
          SVN_ERR (validate_tree (txn_root, expected_trees[j].entries,
                                  expected_trees[j].num_entries));

          /* We don't really want to do anything with this
             transaction...so we'll abort it (good for software, bad
             bad bad for society). */
          svn_fs_abort_txn (txn);
        }
    }

  return SVN_NO_ERROR;
}



/* The test table.  */

svn_error_t * (*test_funcs[]) (const char **msg) = {
  0,
  create_berkeley_filesystem,
  open_berkeley_filesystem,
  trivial_transaction,
  reopen_trivial_transaction,
  create_file_transaction,
  verify_txn_list,
  call_functions_with_unopened_fs,
  write_and_read_file,
  create_mini_tree_transaction,
  create_greek_tree_transaction,
  list_directory,
  revision_props,
  node_props,
  delete_mutables,
  delete,
  abort_txn,
  test_tree_node_validation,
  fetch_by_id,
  merge_trees,
  fetch_youngest_rev,
  basic_commit,
  copy_test,
  merging_commit,
  dir_deltas,
  0
};



/* 
 * local variables:
 * eval: (load-file "../../svn-dev.el")
 * end:
 */
