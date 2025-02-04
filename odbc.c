/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (c)  2002-2025, University of Amsterdam,
			      VU University Amsterdam,
			      SWI-Prolog Solutions b.v.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module is based on pl_odbc.{c,pl},   a  read-only ODBC interface by
Stefano  De  Giorgi  (s.degiorgi@tin.it).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define _CRT_SECURE_NO_WARNINGS 1
#include <config.h>
#ifdef _MSC_VER
#pragma warning(disable : 4996) /* deprecated function SQLSetConnectOption() */
#endif

#include <SWI-Stream.h>
#include <SWI-Prolog.h>
#ifdef __WINDOWS__
#include <windows.h>
#endif

#define O_DEBUG 1

static int odbc_debuglevel = 0;

#ifdef O_DEBUG
#define DEBUG(level, g) if ( odbc_debuglevel >= (level) ) g
#else
#define DEBUG(level, g) ((void)0)
#endif

#include <sql.h>
#include <sqlext.h>
#include <time.h>
#include <limits.h>			/* LONG_MAX, etc. */
#include <math.h>

#ifndef HAVE_SQLLEN
#define SQLLEN DWORD
#endif
#ifndef HAVE_SQLULEN
#define SQLULEN SQLUINTEGER
#endif

#ifndef SQL_COPT_SS_MARS_ENABLED
#define SQL_COPT_SS_MARS_ENABLED 1224
#endif

#ifndef SQL_MARS_ENABLED_YES
#define SQL_MARS_ENABLED_YES (SQLPOINTER)1
#endif

#ifndef WORDS_BIGENDIAN
#define ENC_SQLWCHAR ENC_UNICODE_LE
#else
#define ENC_SQLWCHAR ENC_UNICODE_BE
#endif

#ifdef __WINDOWS__
#define DEFAULT_ENCODING ENC_SQLWCHAR
#else
#define DEFAULT_ENCODING ENC_UTF8
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifndef NULL
#define NULL 0
#endif
#define MAX_NOGETDATA 1024		/* use SQLGetData() on wider columns */
#ifndef STRICT
#define STRICT
#endif

#define NameBufferLength 256
#define CVNERR -1			/* conversion error */

#if defined(_REENTRANT) && defined(O_PLMT)
#include <pthread.h>

					/* FIXME: Actually use these */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&mutex)
#define UNLOCK() pthread_mutex_unlock(&mutex)
#if __WINDOWS__
static CRITICAL_SECTION context_mutex;
#define INIT_CONTEXT_LOCK() InitializeCriticalSection(&context_mutex)
#define LOCK_CONTEXTS()	EnterCriticalSection(&context_mutex)
#define UNLOCK_CONTEXTS() LeaveCriticalSection(&context_mutex)
#else
static pthread_mutex_t context_mutex = PTHREAD_MUTEX_INITIALIZER;
#define INIT_CONTEXT_LOCK()
#define LOCK_CONTEXTS() pthread_mutex_lock(&context_mutex)
#define UNLOCK_CONTEXTS() pthread_mutex_unlock(&context_mutex)
#endif
#else /*multi-threaded*/
#define LOCK()
#define UNLOCK()
#define LOCK_CONTEXTS()
#define UNLOCK_CONTEXTS()
#define INIT_CONTEXT_LOCK()
#endif /*multi-threaded*/

#if !defined(HAVE_TIMEGM) && defined(HAVE_MKTIME) && defined(USE_UTC)
#define EMULATE_TIMEGM
static time_t timegm(struct tm *tm);
#define HAVE_TIMEGM
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Work around bug in MS SQL Server  that doesn't allow for SQLGetData() on
SQLColumns(). Grrr!
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define SQL_SERVER_BUG	1

static atom_t    ATOM_row;		/* "row" */
static atom_t    ATOM_informational;	/* "informational" */
static atom_t	 ATOM_default;		/* "default" */
static atom_t	 ATOM_once;		/* "once" */
static atom_t	 ATOM_multiple;		/* "multiple" */
static atom_t	 ATOM_commit;		/* "commit" */
static atom_t	 ATOM_rollback;		/* "rollback" */
static atom_t	 ATOM_atom;
static atom_t	 ATOM_string;
static atom_t	 ATOM_codes;
static atom_t	 ATOM_float;
static atom_t	 ATOM_integer;
static atom_t	 ATOM_time;
static atom_t	 ATOM_date;
static atom_t	 ATOM_timestamp;
static atom_t	 ATOM_all_types;
static atom_t	 ATOM_null;		/* default null atom */
static atom_t	 ATOM_;			/* "" */
static atom_t	 ATOM_read;
static atom_t	 ATOM_update;
static atom_t    ATOM_dynamic;
static atom_t	 ATOM_forwards_only;
static atom_t	 ATOM_keyset_driven;
static atom_t	 ATOM_static;
static atom_t	 ATOM_auto;
static atom_t	 ATOM_fetch;
static atom_t	 ATOM_end_of_file;
static atom_t	 ATOM_next;
static atom_t	 ATOM_prior;
static atom_t	 ATOM_first;
static atom_t	 ATOM_last;
static atom_t	 ATOM_absolute;
static atom_t	 ATOM_relative;
static atom_t	 ATOM_bookmark;
static atom_t	 ATOM_strict;
static atom_t	 ATOM_relaxed;

static functor_t FUNCTOR_timestamp7;	/* timestamp/7 */
static functor_t FUNCTOR_time3;		/* time/7 */
static functor_t FUNCTOR_date3;		/* date/3 */
static functor_t FUNCTOR_odbc3;		/* odbc(state, code, message) */
static functor_t FUNCTOR_error2;	/* error(Formal, Context) */
static functor_t FUNCTOR_type_error2;	/* type_error(Term, Expected) */
static functor_t FUNCTOR_domain_error2;	/* domain_error(Term, Expected) */
static functor_t FUNCTOR_existence_error2; /* existence_error(Term, Expected) */
static functor_t FUNCTOR_representation_error1; /* representation_error(What) */
static functor_t FUNCTOR_resource_error1; /* resource_error(Error) */
static functor_t FUNCTOR_permission_error3;
static functor_t FUNCTOR_odbc_statement1; /* $odbc_statement(Id) */
static functor_t FUNCTOR_odbc_connection1;
static functor_t FUNCTOR_encoding1;
static functor_t FUNCTOR_user1;
static functor_t FUNCTOR_password1;
static functor_t FUNCTOR_driver_string1;
static functor_t FUNCTOR_alias1;
static functor_t FUNCTOR_mars1;
static functor_t FUNCTOR_connection_pooling1;
static functor_t FUNCTOR_connection_pool_mode1;
static functor_t FUNCTOR_odbc_version1;
static functor_t FUNCTOR_open1;
static functor_t FUNCTOR_auto_commit1;
static functor_t FUNCTOR_types1;
static functor_t FUNCTOR_minus2;
static functor_t FUNCTOR_gt2;
static functor_t FUNCTOR_context_error3;
static functor_t FUNCTOR_data_source2;
static functor_t FUNCTOR_null1;
static functor_t FUNCTOR_source1;
static functor_t FUNCTOR_column3;
static functor_t FUNCTOR_access_mode1;
static functor_t FUNCTOR_cursor_type1;
static functor_t FUNCTOR_silent1;
static functor_t FUNCTOR_findall2;	/* findall(Term, row(...)) */
static functor_t FUNCTOR_affected1;
static functor_t FUNCTOR_fetch1;
static functor_t FUNCTOR_wide_column_threshold1;	/* set max_nogetdata */

#define SQL_PL_DEFAULT  0		/* don't change! */
#define SQL_PL_ATOM	1		/* return as atom */
#define SQL_PL_CODES	2		/* return as code-list */
#define SQL_PL_STRING	3		/* return as string */
#define SQL_PL_INTEGER	4		/* return as integer */
#define SQL_PL_FLOAT	5		/* return as float */
#define SQL_PL_TIME	6		/* return as time/3 structure */
#define SQL_PL_DATE	7		/* return as date/3 structure */
#define SQL_PL_TIMESTAMP 8		/* return as timestamp/7 structure */

#define PARAM_BUFSIZE (SQLLEN)sizeof(double)

typedef uintptr_t code;

typedef struct
{ SWORD        cTypeID;			/* C type of value */
  SWORD	       plTypeID;		/* Prolog type of value */
  SWORD	       sqlTypeID;		/* Sql type of value */
  SWORD	       scale;			/* Scale */
  SQLPOINTER   ptr_value;		/* ptr to value */
  SQLLEN       length_ind;		/* length/indicator of value */
  SQLLEN       len_value;		/* length of value (as parameter)  */
  term_t       put_data;		/* data to put there */
  struct
  { atom_t table;			/* Table name */
    atom_t column;			/* column name */
  } source;				/* origin of the data */
  char	       buf[PARAM_BUFSIZE];	/* Small buffer for simple cols */
} parameter;

typedef struct
{ enum
  { NULL_VAR,				/* represent as variable */
    NULL_ATOM,				/* some atom */
    NULL_FUNCTOR,			/* e.g. null(_) */
    NULL_RECORD				/* an arbitrary term */
  } nulltype;
  union
  { atom_t atom;			/* as atom */
    functor_t functor;			/* as functor */
    record_t record;			/* as term */
  } nullvalue;
  int references;			/* reference count */
} nulldef;				/* Prolog's representation of NULL */

typedef struct
{ int references;			/* reference count */
  unsigned flags;			/* misc flags */
  code     codes[1];			/* executable code */
} findall;

typedef struct connection
{ long	       magic;			/* magic code */
  atom_t       alias;			/* alias name of the connection */
  atom_t       dsn;			/* DSN name of the connection */
  HDBC	       hdbc;			/* ODBC handle */
  nulldef     *null;			/* Prolog null value */
  unsigned     flags;			/* general flags */
  int	       max_qualifier_length;	/* SQL_MAX_QUALIFIER_NAME_LEN */
  SQLULEN      max_nogetdata;		/* handle as long field if larger */
  IOENC	       encoding;		/* Character encoding to use */
  int	       rep_flag;		/* REP_* for encoding */
  struct connection *next;		/* next in chain */
} connection;

typedef struct
{ long	       magic;			/* magic code */
  connection  *connection;		/* connection used */
  HENV	       henv;			/* ODBC environment */
  HSTMT	       hstmt;			/* ODBC statement handle */
  RETCODE      rc;			/* status of last operation */
  parameter   *params;			/* Input parameters */
  parameter   *result;			/* Outputs (row descriptions) */
  SQLSMALLINT  NumCols;			/* # columns */
  SQLSMALLINT  NumParams;		/* # parameters */
  functor_t    db_row;			/* Functor for row */
  SQLINTEGER   sqllen;			/* length of statement (in characters) */
  union
  { SQLWCHAR  *w;			/* as unicode */
    unsigned char *a;			/* as multibyte */
  } sqltext;				/* statement text */
  int	       char_width;		/* sizeof a character */
  unsigned     flags;			/* general flags */
  nulldef     *null;			/* Prolog null value */
  findall     *findall;			/* compiled code to create result */
  SQLULEN      max_nogetdata;		/* handle as long field if larger */
  struct context *clones;		/* chain of clones */
} context;

static struct
{ long	statements_created;		/* # created statements */
  long  statements_freed;		/* # destroyed statements */
} statistics;


#define CON_MAGIC      0x7c42b620	/* magic code */
#define CTX_MAGIC      0x7c42b621	/* magic code */
#define CTX_FREEMAGIC  0x7c42b622	/* magic code if freed */

#define CTX_PERSISTENT  0x0001		/* persistent statement handle */
#define CTX_BOUND       0x0002		/* result-columns are bound */
#define	CTX_SQLMALLOCED 0x0004		/* sqltext is malloced */
#define CTX_INUSE	0x0008		/* statement is running */
#define CTX_OWNNULL	0x0010		/* null-definition is not shared */
#define CTX_SOURCE	0x0020		/* include source of results */
#define CTX_SILENT	0x0040		/* don't produce messages */
#define CTX_PREFETCHED	0x0080		/* we have a prefetched value */
#define CTX_COLUMNS	0x0100		/* this is an SQLColumns() statement */
#define CTX_TABLES	0x0200		/* this is an SQLTables() statement */
#define CTX_GOT_QLEN	0x0400		/* got SQL_MAX_QUALIFIER_NAME_LEN */
#define CTX_NOAUTO	0x0800		/* fetch by hand */

#define CTX_PRIMARYKEY	0x1000		/* this is an SQLPrimaryKeys() statement */
#define CTX_FOREIGNKEY	0x2000		/* this is an SQLForeignKeys() statement */
#define CTX_EXECUTING	0x4000		/* Context is currently being used in SQLExecute */

#define FND_SIZE(n)	((size_t)&((findall*)NULL)->codes[n])

#define ison(s, f)	((s)->flags & (f))
#define isoff(s, f)	!ison(s, f)
#define set(s, f)	((s)->flags |= (f))
#define clear(s, f)	((s)->flags &= ~(f))

static  HENV henv;			/* environment handle (ODBC) */


/* Prototypes */
static int pl_put_row(term_t, context *);
static int pl_put_column(context *c, int nth, term_t col);
static SWORD CvtSqlToCType(context *ctxt, SQLSMALLINT, SQLSMALLINT);
static void free_context(context *ctx);
static void close_context(context *ctx);
static void unmark_and_close_context(context *ctx);
static foreign_t odbc_set_connection(connection *cn, term_t option);
static int get_pltype(term_t t, SWORD *type);
static SWORD get_sqltype_from_atom(atom_t name, SWORD *type);
static const char *sql_type_name(SWORD type);
static const char *sql_c_type_name(SWORD type);


		 /*******************************
		 *	      ERRORS		*
		 *******************************/

static int
odbc_report(HENV henv, HDBC hdbc, HSTMT hstmt, RETCODE rc)
{ SQLCHAR state[16];			/* Normally 5-character ID */
  SQLINTEGER native;			/* was DWORD */
  SQLCHAR message[SQL_MAX_MESSAGE_LENGTH+1];
  SWORD   msglen;
  RETCODE rce;
  term_t  msg = PL_new_term_ref();

  switch ( (rce=SQLError(henv, hdbc, hstmt, state, &native, message,
			 sizeof(message), &msglen)) )
  { case SQL_NO_DATA_FOUND:
    case SQL_SUCCESS_WITH_INFO:
      if ( rc != SQL_ERROR )
	return TRUE;
      /*FALLTHROUGH*/
    case SQL_SUCCESS:
    { term_t s;

      if ( msglen > SQL_MAX_MESSAGE_LENGTH )
	msglen = SQL_MAX_MESSAGE_LENGTH; /* TBD: get the rest? */

      if ( (s = PL_new_term_ref()) &&
	   PL_unify_chars(s, PL_STRING|REP_MB,
			  (size_t)msglen, (const char*)message) &&
	   PL_unify_term(msg,
			 PL_FUNCTOR, FUNCTOR_odbc3,
			   PL_CHARS,   state,
			   PL_INTEGER, (long)native,
			   PL_TERM,    s) )
	break;

      return FALSE;
    }
    case SQL_INVALID_HANDLE:
      return PL_warning("ODBC INTERNAL ERROR: Invalid handle in error");
    default:
      if ( rc != SQL_ERROR )
	return TRUE;
  }

  switch(rc)
  { case SQL_SUCCESS_WITH_INFO:
    { fid_t fid = PL_open_foreign_frame();
      predicate_t pred = PL_predicate("print_message", 2, "user");
      term_t av;
      int rc;

      rc = ( (av = PL_new_term_refs(2)) &&
	     PL_put_atom(av+0, ATOM_informational) &&
	     PL_put_term(av+1, msg) &&
	     PL_call_predicate(NULL, PL_Q_NORMAL, pred, av)
	   );
      PL_discard_foreign_frame(fid);

      return rc;
    }
    case SQL_ERROR:
    { term_t ex;

      if ( (ex=PL_new_term_ref()) &&
	   PL_unify_term(ex,
			 PL_FUNCTOR, FUNCTOR_error2,
			   PL_TERM, msg,
			 PL_VARIABLE) )
	return PL_raise_exception(ex);

      return FALSE;
    }
    default:
      return PL_warning("Statement returned %d\n", rc);
  }
}

#define TRY(ctxt, stmt, onfail) \
	{ ctxt->rc = (stmt); \
	  if ( !report_status(ctxt) ) \
	  { onfail; \
	    return FALSE; \
	  } \
	}


static int
report_status(context *ctxt)
{ switch(ctxt->rc)
  { case SQL_SUCCESS:
      return TRUE;
    case SQL_SUCCESS_WITH_INFO:
      if ( ison(ctxt, CTX_SILENT) )
	return TRUE;
      break;
    case SQL_NO_DATA_FOUND:
      return TRUE;
    case SQL_INVALID_HANDLE:
      return PL_warning("Invalid handle: %p", ctxt->hstmt);
  }

  return odbc_report(ctxt->henv, ctxt->connection->hdbc,
		     ctxt->hstmt, ctxt->rc);
}


static int
type_error(term_t actual, const char *expected)
{ term_t ex;

  if ( (ex=PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR, FUNCTOR_error2,
		       PL_FUNCTOR, FUNCTOR_type_error2,
			 PL_CHARS, expected,
			 PL_TERM, actual,
		       PL_VARIABLE) )
    return PL_raise_exception(ex);

  return FALSE;
}

static int
domain_error(term_t actual, const char *expected)
{ term_t ex;

  if ( (ex=PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR, FUNCTOR_error2,
		       PL_FUNCTOR, FUNCTOR_domain_error2,
			 PL_CHARS, expected,
			 PL_TERM, actual,
		       PL_VARIABLE) )
    return PL_raise_exception(ex);

  return FALSE;
}

static int
existence_error(term_t actual, const char *expected)
{ term_t ex;

  if ( (ex=PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR, FUNCTOR_error2,
		       PL_FUNCTOR, FUNCTOR_existence_error2,
			 PL_CHARS, expected,
			 PL_TERM, actual,
		       PL_VARIABLE) )
    return PL_raise_exception(ex);

  return FALSE;
}

static int
resource_error(const char *error)
{ term_t ex;

  if ( (ex=PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR, FUNCTOR_error2,
		       PL_FUNCTOR, FUNCTOR_resource_error1,
			 PL_CHARS, error,
		       PL_VARIABLE) )
    return PL_raise_exception(ex);

  return FALSE;
}


static int
representation_error(term_t t, const char *error)
{ term_t ex;

  if ( (ex=PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR, FUNCTOR_error2,
		       PL_FUNCTOR, FUNCTOR_representation_error1,
			 PL_CHARS, error,
		       PL_TERM, t) )
    return PL_raise_exception(ex);

  return FALSE;
}


static int
context_error(term_t term, const char *error, const char *what)
{ term_t ex;

  if ( (ex=PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR, FUNCTOR_error2,
		       PL_FUNCTOR, FUNCTOR_context_error3,
			 PL_TERM, term,
			 PL_CHARS, error,
			 PL_CHARS, what,
		       PL_VARIABLE) )
    return PL_raise_exception(ex);

  return FALSE;
}


static int
permission_error(const char *op, const char *type, term_t obj)
{ term_t ex;

  if ( (ex=PL_new_term_ref()) &&
       PL_unify_term(ex,
		     PL_FUNCTOR, FUNCTOR_error2,
		       PL_FUNCTOR, FUNCTOR_permission_error3,
			 PL_CHARS, op,
			 PL_CHARS, type,
			 PL_TERM, obj,
		       PL_VARIABLE) )
    return PL_raise_exception(ex);

  return FALSE;
}


static void *
odbc_malloc(size_t bytes)
{ void *ptr = malloc(bytes);

  if ( !ptr )
    resource_error("memory");

  return ptr;
}


static void *
odbc_realloc(void* inptr, size_t bytes)
{ void *ptr = realloc(inptr, bytes);

  if ( !ptr )
  { free(inptr);
    resource_error("memory");
  }

  return ptr;
}


		 /*******************************
		 *	     PRIMITIVES		*
		 *******************************/

typedef int (*AtypeFunc)(term_t t, void *vp);

#define get_name_arg_ex(i, t, n)  \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)PL_get_atom_chars, "atom", n)
#define get_text_arg_ex(i, t, n)  \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)get_text, "text", n)
#define get_atom_arg_ex(i, t, n)  \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)PL_get_atom, "atom", n)
#define get_int_arg_ex(i, t, n)   \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)PL_get_integer, "integer", n)
#define get_long_arg_ex(i, t, n)   \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)PL_get_long, "integer", n)
#define get_bool_arg_ex(i, t, n)   \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)PL_get_bool, "boolean", n)
#define get_float_arg_ex(i, t, n) \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)PL_get_float, "float", n)
#define get_encoding_arg_ex(i, t, n) \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)get_encoding, "encoding", n)
#define get_odbc_version_arg_ex(i, t, n) \
	PL_get_typed_arg_ex(i, t, (AtypeFunc)get_odbc_version, "odbc_version", n)

/* Used for passwd and driver string.  Should use Unicode/encoding
   stuff for that.
*/

static int
get_text(term_t t, char **s)
{ return PL_get_chars(t, s, CVT_ATOM|CVT_STRING|CVT_LIST|REP_MB|BUF_RING);
}

typedef struct enc_name
{ char	       *name;
  IOENC		code;
  atom_t	a;
} enc_name;

static enc_name encodings[] =
{ { "iso_latin_1", ENC_ISO_LATIN_1 },
  { "locale",	   ENC_ANSI },
  { "utf8",	   ENC_UTF8 },
  { "unicode",     ENC_SQLWCHAR },
  { NULL }
};


static int
get_encoding(term_t t, IOENC *enc)
{ atom_t a;

  if ( PL_get_atom(t, &a) )
  { enc_name *en;

    for(en=encodings; en->name; en++)
    { if ( !en->a )
	en->a = PL_new_atom(en->name);
      if ( en->a == a )
      { *enc = en->code;
	return TRUE;
      }
    }
  }

  return FALSE;
}


static void
put_encoding(term_t t, IOENC enc)
{ enc_name *en;

  for(en=encodings; en->name; en++)
  { if ( en->code == enc )
    { if ( !en->a )
	en->a = PL_new_atom(en->name);
      PL_put_atom(t, en->a);
      return;
    }
  }

  assert(0);
}


static int
enc_to_rep(IOENC enc)
{ switch(enc)
  { case ENC_ISO_LATIN_1:
      return REP_ISO_LATIN_1;
    case ENC_ANSI:
      return REP_MB;
    case ENC_UTF8:
      return REP_UTF8;
    case ENC_SQLWCHAR:
      return 0;				/* not used for wide characters */
    default:
      assert(0);
      return 0;
  }
}


static int
PL_get_typed_arg_ex(int i, term_t t, AtypeFunc func, const char *ex, void *ap)
{ term_t a = PL_new_term_ref();

  if ( !PL_get_arg(i, t, a) )
    return type_error(t, "compound");
  if ( !(*func)(a, ap) )
    return type_error(a, ex);

  return TRUE;
}

#define get_int_arg(i, t, n)   \
	PL_get_typed_arg(i, t, (AtypeFunc)PL_get_integer, n)

static int
PL_get_typed_arg(int i, term_t t, AtypeFunc func, void *ap)
{ term_t a = PL_new_term_ref();

  if ( !PL_get_arg(i, t, a) )
    return FALSE;
  return (*func)(a, ap);
}


static int
list_length(term_t list)
{ size_t len;

  if ( PL_skip_list(list, 0, &len) == PL_LIST )
    return (int)len;

  type_error(list, "list");
  return -1;
}


typedef struct odbc_version_name
{ char	       *name;
  intptr_t	version;
  atom_t	a;
} odbc_version_name;

static odbc_version_name odbc_versions[] =
{ { "2.0",	SQL_OV_ODBC2 },
  { "3.0",	SQL_OV_ODBC3 },
  { NULL }
};

static int
get_odbc_version(term_t t, intptr_t *ver)
{ atom_t a;

  if ( PL_get_atom_ex(t, &a) )
  { odbc_version_name *v;

    for(v=odbc_versions; v->name; v++)
    { if ( !v->a )
	v->a = PL_new_atom(v->name);
      if ( v->a == a )
      { *ver = v->version;
	return TRUE;
      }
    }
  }

  return FALSE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int formatted_string(Context, +Fmt-[Arg...])
    Much like sformat, but this approach avoids avoids creating
    intermediate Prolog data.  Maybe we should publish pl_format()?
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
formatted_string(context *ctxt, term_t in)
{ term_t av = PL_new_term_refs(3);
  static predicate_t format;
  char *out = NULL;
  size_t len = 0;
  IOSTREAM *fd = Sopenmem(&out, &len, "w");

  if ( !fd )
    return FALSE;			/* resource error */
  if ( !format )
    format = PL_predicate("format", 3, "user");

  fd->encoding = ctxt->connection->encoding;
  if ( !PL_unify_stream(av+0, fd) ||
       !PL_get_arg(1, in, av+1) ||
       !PL_get_arg(2, in, av+2) ||
       !PL_call_predicate(NULL, PL_Q_PASS_EXCEPTION, format, av) )
  { Sclose(fd);
    if ( out )
      PL_free(out);
    return FALSE;
  }
  Sclose(fd);

  if ( ctxt->connection->encoding == ENC_SQLWCHAR )
  { ctxt->sqltext.w = (SQLWCHAR*)out;
    ctxt->sqllen = (SQLINTEGER)(len/sizeof(SQLWCHAR));	/* TBD: Check range */
    ctxt->char_width = sizeof(SQLWCHAR);
  } else
  { ctxt->sqltext.a = (unsigned char*)out;
    ctxt->sqllen = (SQLINTEGER)len;			/* TBD: Check range */
    ctxt->char_width = sizeof(char);
  }
  set(ctxt, CTX_SQLMALLOCED);

  return TRUE;
}


		 /*******************************
		 *	    NULL VALUES		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
There are many ways one  may  wish   to  handle  SQL  null-values. These
functions deal with the three common ways   specially  and can deal with
arbitrary representations.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static nulldef *
nulldef_spec(term_t t)
{ atom_t a;
  functor_t f;
  nulldef *nd;

  if ( !(nd=odbc_malloc(sizeof(*nd))) )
    return NULL;

  memset(nd, 0, sizeof(*nd));

  if ( PL_get_atom(t, &a) )
  { if ( a == ATOM_null )
    { free(nd);				/* TBD: not very elegant */
      return NULL;			/* default specifier */
    }
    nd->nulltype  = NULL_ATOM;
    nd->nullvalue.atom = a;
    PL_register_atom(a);		/* avoid atom-gc */
  } else if ( PL_is_variable(t) )
  { nd->nulltype = NULL_VAR;
  } else if ( PL_get_functor(t, &f) &&
	      PL_functor_arity(f) == 1 )
  { term_t a1 = PL_new_term_ref();

    _PL_get_arg(1, t, a1);
    if ( PL_is_variable(a1) )
    { nd->nulltype = NULL_FUNCTOR;
      nd->nullvalue.functor = f;
    } else
      goto term;
  } else
  { term:
    nd->nulltype = NULL_RECORD;
    nd->nullvalue.record = PL_record(t);
  }

  nd->references = 1;

  return nd;
}


static nulldef *
clone_nulldef(nulldef *nd)
{ if ( nd )
    nd->references++;

  return nd;
}


static void
free_nulldef(nulldef *nd)
{ if ( nd && --nd->references == 0 )
  { switch(nd->nulltype)
    { case NULL_ATOM:
	PL_unregister_atom(nd->nullvalue.atom);
	break;
      case NULL_RECORD:
	PL_erase(nd->nullvalue.record);
	break;
      default:
	break;
    }

    free(nd);
  }
}


WUNUSED static int
put_sql_null(term_t t, nulldef *nd)
{ if ( nd )
  { switch(nd->nulltype)
    { case NULL_VAR:
	return TRUE;
      case NULL_ATOM:
	return PL_put_atom(t, nd->nullvalue.atom);
      case NULL_FUNCTOR:
	return PL_put_functor(t, nd->nullvalue.functor);
      case NULL_RECORD:
	return PL_recorded(nd->nullvalue.record, t);
      default:
	assert(0);
	return FALSE;
    }
  } else
    return PL_put_atom(t, ATOM_null);
}


static int
is_sql_null(term_t t, nulldef *nd)
{ if ( nd )
  { switch(nd->nulltype)
    { case NULL_VAR:
	return PL_is_variable(t);
      case NULL_ATOM:
      { atom_t a;

	return PL_get_atom(t, &a) && a == nd->nullvalue.atom;
      }
      case NULL_FUNCTOR:
	return PL_is_functor(t, nd->nullvalue.functor);
      case NULL_RECORD:			/* TBD: Provide PL_unify_record */
      { term_t rec = PL_new_term_ref();
	PL_recorded(nd->nullvalue.record, rec);
	return PL_unify(t, rec);
      }
      default:				/* should not happen */
	assert(0);
	return FALSE;
    }
  } else
  { atom_t a;

    return PL_get_atom(t, &a) && a == ATOM_null;
  }
}

		 /*******************************
		 *   FINDALL(Term, row(X,...))	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This section deals with  the  implementation   of  the  statement option
findall(Template, row(Column,...)), returning a  list   of  instances of
Template for each row.

Ideally, we should unify the row with   the  second argument and add the
first to the list. Unfortunately, we have   to  make fresh copies of the
findall/2 term for this to work, or we must protect the Template using a
record. Both approaches are slow and largely  discard the purpose of the
option, which is to avoid findall/3 and its associated costs in terms of
copying and memory fragmentation.

The current implementation is incomplete. It does not allow arguments of
row(...) to be instantiated. Plain instantiation   can always be avoided
using a proper SELECT statement. Potentionally   useful however would be
the  translation  of   compound   terms,    especially   to   translates
date/time/timestamp structures to a format for use by the application.

The  statement  is  compiled  into  a    findall  statement,  a  set  of
instructions that builds the target structure   from the row returned by
the current statement.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define MAXCODES 256
#define ROW_ARG  1024			/* way above Prolog types */

typedef struct
{ term_t row;				/* the row */
  term_t tmp;				/* scratch term */
  size_t columns;				/* arity of row-term */
  unsigned flags;			/* CTX_PERSISTENT */
  int  size;				/* # codes */
  code buf[MAXCODES];
} compile_info;

#define ADDCODE(info, val) (info->buf[info->size++] = (code)(val))
#define ADDCODE_1(info, v1, v2) ADDCODE(info, v1), ADDCODE(info, v2)

static int
nth_row_arg(compile_info *info, term_t var)
{ int i;

  for(i=1; i<=info->columns; i++)
  { _PL_get_arg(i, info->row, info->tmp);
    if ( PL_compare(info->tmp, var) == 0 )
      return i;
  }

  return 0;
}


typedef union
{ code   ascode[sizeof(double)/sizeof(code)];
  double asdouble;
} u_double;


static int
compile_arg(compile_info *info, term_t t)
{ int tt;

  switch((tt=PL_term_type(t)))
  { case PL_VARIABLE:
    { int nth;

      if ( (nth=nth_row_arg(info, t)) )
      { ADDCODE_1(info, ROW_ARG, nth);
      } else
	ADDCODE(info, PL_VARIABLE);
      break;
    }
    case PL_ATOM:
#ifdef PL_NIL
    case PL_NIL:
#endif
    { atom_t val;

      if ( !PL_get_atom(t, &val) )
	assert(0);
      ADDCODE_1(info, PL_ATOM, val);
      if ( ison(info, CTX_PERSISTENT) )
	PL_register_atom(val);
      break;
    }
    case PL_STRING:
    case PL_FLOAT:
      if ( ison(info, CTX_PERSISTENT) )
      { if ( tt == PL_FLOAT )
	{ u_double v;
	  unsigned int i;

	  if ( !PL_get_float(t, &v.asdouble) )
	    assert(0);
	  ADDCODE(info, PL_FLOAT);
	  for(i=0; i<sizeof(double)/sizeof(code); i++)
	    ADDCODE(info, v.ascode[i]);
	} else				/* string */
	{ size_t len;
	  char *s, *cp = NULL;
	  wchar_t *w = NULL;
	  int flags = 0;

	  if (PL_get_string_chars(t, &s, &len))
	  { if ( !(cp = odbc_malloc(len+1)) )
	      return FALSE;
	    memcpy(cp, s, len+1);
	  } else if (PL_get_wchars(t, &len, &w, CVT_STRING|CVT_EXCEPTION))
	  { if ( !(cp = odbc_malloc((len+1)*sizeof(wchar_t))) )
	      return FALSE;
	    memcpy(cp, w, (len+1)*sizeof(wchar_t));
	    flags |= PL_BLOB_WCHAR;
	  } else {
	    return FALSE;
	  }
	  ADDCODE(info, PL_STRING);
	  ADDCODE(info, flags);
	  ADDCODE(info, len);
	  ADDCODE(info, cp);
	}
      } else
      { term_t cp = PL_copy_term_ref(t);
	ADDCODE_1(info, PL_TERM, cp);
      }
      break;
    case PL_INTEGER:
    { int64_t v;

      if ( !PL_get_int64(t, &v) )
	return PL_domain_error("int64", t);
      ADDCODE_1(info, PL_INTEGER, v);
      break;
    }
    case PL_TERM:
#ifdef PL_LIST_PAIR
    case PL_LIST_PAIR:
#endif
    { functor_t f;
      size_t i, arity;
      term_t a = PL_new_term_ref();

      if ( !PL_get_functor(t, &f) )
	assert(0);
      arity = PL_functor_arity(f);
      ADDCODE_1(info, PL_FUNCTOR, f);
      for(i=1; i<=arity; i++)
      { _PL_get_arg(i, t, a);
	if ( !compile_arg(info, a) )
	  return FALSE;
      }
      break;
    }
    default:
      assert(0);
  }

  return TRUE;
}


static findall *
compile_findall(term_t all, unsigned flags)
{ compile_info info;
  term_t t = PL_new_term_ref();
  atom_t a;
  findall *f;
  int i;

  info.tmp   = PL_new_term_ref();
  info.row   = PL_new_term_ref();
  info.size  = 0;
  info.flags = flags;

  if ( !PL_get_arg(2, all, info.row) ||
       !PL_get_name_arity(info.row, &a, &info.columns) )
    return NULL;

  for(i=1; i<=info.columns; i++)
  { if ( !PL_get_arg(i, info.row, t) )
      return NULL;
    if ( !PL_is_variable(t) )
    { type_error(t, "unbound");
      return NULL;
    }
  }

  if ( !PL_get_arg(1, all, t) )
    return NULL;
  if ( !compile_arg(&info, t) )
    return NULL;

  if ( !(f = odbc_malloc(FND_SIZE(info.size))) )
    return NULL;
  f->references = 1;
  f->flags = flags;
  memcpy(f->codes, info.buf, sizeof(code)*info.size);

  return f;
}


static findall *
clone_findall(findall *in)
{ if ( in )
    in->references++;
  return in;
}


static code *
unregister_code(code *PC)
{ switch((int)*PC++)
  { case PL_VARIABLE:
      return PC;
    case ROW_ARG:			/* 1-based column */
    case PL_INTEGER:
    case PL_TERM:
      return PC+1;
    case PL_ATOM:
      PL_unregister_atom((atom_t)*PC++);
      return PC;
    case PL_FLOAT:
      return PC+sizeof(double)/sizeof(code);
    case PL_STRING:
    { char *s = (char*)PC[2];
      free(s);
      return PC+3;
    }
    case PL_FUNCTOR:
    { functor_t f = (functor_t)*PC++;
      size_t i, arity = PL_functor_arity(f);

      for(i=0;i<arity;i++)
      { if ( !(PC=unregister_code(PC)) )
	  return NULL;
      }

      return PC;
    }
    default:
      assert(0);
      return NULL;
  }
}


static void
free_findall(findall *in)
{ if ( in && --in->references == 0 )
  { if ( ison(in, CTX_PERSISTENT) )
      unregister_code(in->codes);

    free(in);
  }
}


static code *
build_term(context *ctxt, code *PC, term_t result)
{ switch((int)*PC++)
  { case PL_VARIABLE:
      return PC;
    case ROW_ARG:			/* 1-based column */
    { int column = (int)*PC++;
      if ( pl_put_column(ctxt, column-1, result) )
	return PC;
      return NULL;
    }
    case PL_ATOM:
    { PL_put_atom(result, (atom_t)*PC++);
      return PC;
    }
    case PL_FLOAT:
    { u_double v;
      unsigned int i;

      for(i=0; i<sizeof(double)/sizeof(code); i++)
	v.ascode[i] = *PC++;
      if ( !PL_put_float(result, v.asdouble) )
	return NULL;
      return PC;
    }
    case PL_STRING:
    { if (((int)*PC++)&PL_BLOB_WCHAR)
      { size_t len = (size_t)*PC++;
	wchar_t *w = (wchar_t*)*PC++;
	if ( !PL_unify_wchars(result, PL_STRING, len, w) )
	  return NULL;
      } else
      { size_t len = (size_t)*PC++;
	char *s = (char*)*PC++;
	if ( !PL_put_string_nchars(result, len, s) )
	  return NULL;
      }
      return PC;
    }
    case PL_INTEGER:
    { if ( !PL_put_int64(result, (int64_t)*PC++) )
	return NULL;
      return PC;
    }
    case PL_TERM:
    { if ( !PL_put_term(result, (term_t)*PC++) )
	return NULL;
      return PC;
    }
    case PL_FUNCTOR:
    { functor_t f = (functor_t)*PC++;
      size_t i, arity = PL_functor_arity(f);
      term_t av = PL_new_term_refs((int)arity);

      for(i=0;i<arity;i++)
      { if ( !(PC=build_term(ctxt, PC, av+i)) )
	  return NULL;
      }

      if ( !PL_cons_functor_v(result, f, av) )
	return NULL;
      PL_reset_term_refs(av);
      return PC;
    }
    default:
      assert(0);
      return NULL;
  }
}


static int
put_findall(context *ctxt, term_t result)
{ PL_put_variable(result);
  if ( build_term(ctxt, ctxt->findall->codes, result) )
    return TRUE;

  return FALSE;
}



		 /*******************************
		 *	    CONNECTION		*
		 *******************************/

static connection *connections;

static connection *
find_connection(atom_t alias)
{ connection *c;

  LOCK();
  for(c=connections; c; c=c->next)
  { if ( c->alias == alias )
    { UNLOCK();
      return c;
    }
  }
  UNLOCK();

  return NULL;
}


static connection *
find_connection_from_dsn(atom_t dsn)
{ connection *c;

  LOCK();
  for(c=connections; c; c=c->next)
  { if ( c->dsn == dsn )
    { UNLOCK();
      return c;
    }
  }
  UNLOCK();

  return NULL;
}


static connection *
alloc_connection(atom_t alias, atom_t dsn)
{ connection *c;

  if ( alias && find_connection(alias) )
    return NULL;			/* already existenting */

  if ( !(c = odbc_malloc(sizeof(*c))) )
    return NULL;
  memset(c, 0, sizeof(*c));
  c->alias = alias;
  c->magic = CON_MAGIC;
  if ( alias )
    PL_register_atom(alias);
  c->dsn = dsn;
  PL_register_atom(dsn);
  c->max_nogetdata = MAX_NOGETDATA;

  LOCK();
  c->next = connections;
  connections = c;
  UNLOCK();

  return c;
}


static void
free_connection(connection *c)
{ LOCK();
  if ( c == connections )
    connections = c->next;
  else
  { connection *c2;

    for(c2 = connections; c2; c2 = c2->next)
    { if ( c2->next == c )
      { c2->next = c->next;
	break;
      }
    }
  }
  UNLOCK();

  if ( c->alias )
    PL_unregister_atom(c->alias);
  if ( c->dsn )
    PL_unregister_atom(c->dsn);
  free_nulldef(c->null);

  free(c);
}


static int
get_connection(term_t tcid, connection **cn)
{ atom_t alias;
  connection *c;

  if ( PL_is_functor(tcid, FUNCTOR_odbc_connection1) )
  { term_t a = PL_new_term_ref();
    void *ptr;

    _PL_get_arg(1, tcid, a);
    if ( !PL_get_pointer(a, &ptr) )
      return type_error(tcid, "odbc_connection");
    c = ptr;

    if ( c->magic != CON_MAGIC )
      return existence_error(tcid, "odbc_connection");
  } else
  { if ( !PL_get_atom(tcid, &alias) )
      return type_error(tcid, "odbc_connection");
    if ( !(c=find_connection(alias)) )
      return existence_error(tcid, "odbc_connection");
  }

  *cn = c;

  return TRUE;
}


static int
unify_connection(term_t t, connection *cn)
{ if ( cn->alias )
    return PL_unify_atom(t, cn->alias);

  return PL_unify_term(t, PL_FUNCTOR, FUNCTOR_odbc_connection1,
			    PL_POINTER, cn);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
odbc_connect(+DSN, -Connection, +Options)
    Create a new connection. Option is a list of options with the
    following standards:

	user(User)

	password(Password)

	alias(Name)
	    Alias-name for the connection.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define MAX_AFTER_OPTIONS 10

static foreign_t
pl_odbc_connect(term_t tdsource, term_t cid, term_t options)
{  atom_t dsn;
   const char *dsource;			/* odbc data source */
   char *uid = NULL;			/* user id */
   char *pwd = NULL;			/* password */
   char *driver_string = NULL;		/* driver_string */
   atom_t alias = 0;			/* alias-name */
   IOENC encoding = DEFAULT_ENCODING;	/* Connection encoding */
   int mars = 0;			/* mars-value */
   atom_t pool_mode = 0;		/* Connection pooling mode */
   intptr_t odbc_version = SQL_OV_ODBC3;	/* ODBC connectivity version */
   atom_t open = 0;			/* open next connection */
   RETCODE rc;				/* result code for ODBC functions */
   HDBC hdbc;
   connection *cn;
   term_t tail = PL_copy_term_ref(options);
   term_t head = PL_new_term_ref();
   term_t after_open = PL_new_term_refs(MAX_AFTER_OPTIONS);
   int i, nafter = 0;
   int silent = FALSE;

   /* Read parameters from terms. */
   if ( !PL_get_atom(tdsource, &dsn) )
     return type_error(tdsource, "atom");

   while(PL_get_list(tail, head, tail))
   { if ( PL_is_functor(head, FUNCTOR_user1) )
     { if ( !get_name_arg_ex(1, head, &uid) )
	 return FALSE;
     } else if ( PL_is_functor(head, FUNCTOR_password1) )
     { if ( !get_text_arg_ex(1, head, &pwd) )
	 return FALSE;
     } else if ( PL_is_functor(head, FUNCTOR_alias1) )
     { if ( !get_atom_arg_ex(1, head, &alias) )
	 return FALSE;
     } else if ( PL_is_functor(head, FUNCTOR_driver_string1) )
     { if ( !get_text_arg_ex(1, head, &driver_string) )
	 return FALSE;
     } else if ( PL_is_functor(head, FUNCTOR_mars1) )
     { if ( !get_bool_arg_ex(1, head, &mars) )
	 return FALSE;
     } else if ( PL_is_functor(head, FUNCTOR_connection_pool_mode1) )
     { if ( !get_atom_arg_ex(1, head, &pool_mode) )
	 return FALSE;
       if ( pool_mode != ATOM_strict && pool_mode != ATOM_relaxed )
	 return domain_error(head, "pool_mode");
     } else if ( PL_is_functor(head, FUNCTOR_odbc_version1) )
     { if ( !get_odbc_version_arg_ex(1, head, &odbc_version) )
	 return FALSE;
     } else if ( PL_is_functor(head, FUNCTOR_open1) )
     { if ( !get_atom_arg_ex(1, head, &open) )
	 return FALSE;
       if ( !(open == ATOM_once ||
	      open == ATOM_multiple) )
	 return domain_error(head, "open_mode");
     } else if ( PL_is_functor(head, FUNCTOR_silent1) )
     { if ( !get_bool_arg_ex(1, head, &silent) )
	 return FALSE;
     } else if ( PL_is_functor(head, FUNCTOR_encoding1) )
     { if ( !get_encoding_arg_ex(1, head, &encoding) )
	 return FALSE;
     } else if ( PL_is_functor(head, FUNCTOR_auto_commit1) ||
		 PL_is_functor(head, FUNCTOR_null1) ||
		 PL_is_functor(head, FUNCTOR_access_mode1) ||
		 PL_is_functor(head, FUNCTOR_cursor_type1) ||
		 PL_is_functor(head, FUNCTOR_wide_column_threshold1) )
     { if ( nafter < MAX_AFTER_OPTIONS )
       { if ( !PL_put_term(after_open+nafter++, head) )
	   return FALSE;
       } else
	 return PL_warning("Too many options"); /* shouldn't happen */
     } else
       return domain_error(head, "odbc_option");
   }
   if ( !PL_get_nil(tail) )
     return type_error(tail, "list");

   if ( !open )
     open = alias ? ATOM_once : ATOM_multiple;
   if ( open == ATOM_once && (cn = find_connection_from_dsn(dsn)) )
   { if ( alias && cn->alias != alias )
     { if ( !cn->alias )
       { if ( !find_connection(alias) )
	 { cn->alias = alias;
	   PL_register_atom(alias);
	 } else
	   return PL_warning("Alias already in use");
       } else
	 return PL_warning("Cannot redefined connection alias");
     }
     return unify_connection(cid, cn);
   }

   dsource = PL_atom_chars(dsn);

   LOCK();
   if ( !henv )
   { if ( (rc=SQLAllocEnv(&henv)) != SQL_SUCCESS )
     { UNLOCK();
       return PL_warning("Could not initialise SQL environment");
     }
     if ( (rc=SQLSetEnvAttr(henv,
			    SQL_ATTR_ODBC_VERSION,
			    (SQLPOINTER) odbc_version,
			    0)) != SQL_SUCCESS )
     { UNLOCK();
       return odbc_report(henv, NULL, NULL, rc);
     }
   }
   UNLOCK();

   if ( (rc=SQLAllocConnect(henv, &hdbc)) != SQL_SUCCESS )
     return odbc_report(henv, NULL, NULL, rc);

   if ( mars )
   { if ( (rc=SQLSetConnectAttr(hdbc,
				SQL_COPT_SS_MARS_ENABLED,
				SQL_MARS_ENABLED_YES,
				SQL_IS_UINTEGER)) != SQL_SUCCESS )
     { SQLFreeConnect(hdbc);
       return odbc_report(henv, NULL, NULL, rc);
     }
   }

   if ( pool_mode )
   { SQLPOINTER pool_arg = (SQLPOINTER)0;
     if (pool_mode == ATOM_strict)
       pool_arg = (SQLPOINTER)SQL_CP_STRICT_MATCH;
     else if (pool_mode == ATOM_relaxed)
       pool_arg = (SQLPOINTER)SQL_CP_RELAXED_MATCH;
     if ( (rc=SQLSetConnectAttr(hdbc,
				SQL_ATTR_CP_MATCH,
				pool_arg,
				SQL_IS_INTEGER)) != SQL_SUCCESS )
     { SQLFreeConnect(hdbc);
       return odbc_report(henv, NULL, NULL, rc);
     }
   }


   /* Connect to a data source. */
   if ( driver_string != NULL )
   { if ( uid != NULL )
     { SQLFreeConnect(hdbc);
       return context_error(options, "Option incompatible with driver_string",
			    "user");
     } else if ( pwd != NULL )
     { SQLFreeConnect(hdbc);
       return context_error(options, "Option incompatible with driver_string",
			    "password");
     } else
     { SQLCHAR connection_out[1025];	/* completed driver string */
       SQLSMALLINT connection_out_len;

       rc = SQLDriverConnect(hdbc,
			     NULL, /* window handle */
			     (SQLCHAR *)driver_string, SQL_NTS,
			     connection_out, 1024,
			     &connection_out_len,
			     SQL_DRIVER_NOPROMPT);
     }
   } else
   { rc = SQLConnect(hdbc, (SQLCHAR *)dsource, SQL_NTS,
			   (SQLCHAR *)uid,     SQL_NTS,
			   (SQLCHAR *)pwd,     SQL_NTS);
   }
   if ( rc == SQL_ERROR )
   { odbc_report(henv, hdbc, NULL, rc);
     SQLFreeConnect(hdbc);
     return FALSE;
   }
   if ( rc != SQL_SUCCESS && !silent && !odbc_report(henv, hdbc, NULL, rc) )
   { SQLFreeConnect(hdbc);
     return FALSE;
   }

   if ( !(cn=alloc_connection(alias, dsn)) )
   { SQLFreeConnect(hdbc);
     return FALSE;
   }
   if ( silent )
     set(cn, CTX_SILENT);

   cn->encoding = encoding;
   cn->rep_flag = enc_to_rep(encoding);
   cn->hdbc     = hdbc;

   if ( !unify_connection(cid, cn) )
   { SQLFreeConnect(hdbc);
     free_connection(cn);
     return FALSE;
   }

   DEBUG(3, Sdprintf("Processing %d `after' options\n", nafter));
   for(i=0; i<nafter; i++)
   { if ( !odbc_set_connection(cn, after_open+i) )
     { SQLFreeConnect(hdbc);
       free_connection(cn);
       return FALSE;
     }
   }

   return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
odbc_disconnect(+Connection)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define TRY_CN(cn, action) \
	{ RETCODE rc = action; \
	  if ( rc != SQL_SUCCESS ) \
	    return odbc_report(henv, cn->hdbc, NULL, rc); \
	}


static foreign_t
pl_odbc_disconnect(term_t conn)
{ connection *cn;

  if ( !get_connection(conn, &cn) )
    return FALSE;

  TRY_CN(cn, SQLDisconnect(cn->hdbc));  /* Disconnect from the data source */
  TRY_CN(cn, SQLFreeConnect(cn->hdbc)); /* Free the connection handle */
  free_connection(cn);

  return TRUE;
}


static int
add_cid_dsn_pair(term_t list, connection *cn)
{ term_t cnterm = PL_new_term_ref();
  term_t head = PL_new_term_ref();

  if ( PL_unify_list(list, head, list) &&
       unify_connection(cnterm, cn) &&
       PL_unify_term(head, PL_FUNCTOR, FUNCTOR_minus2,
			     PL_TERM, cnterm,
			     PL_ATOM, cn->dsn) )
  { PL_reset_term_refs(cnterm);
    return TRUE;
  }

  return FALSE;
}


static foreign_t
odbc_current_connections(term_t cid, term_t dsn, term_t pairs)
{ atom_t dsn_a;
  term_t tail = PL_copy_term_ref(pairs);
  connection *cn;

  if ( !PL_get_atom(dsn, &dsn_a) )
    dsn_a = 0;

  if ( !PL_is_variable(cid) )
  { if ( get_connection(cid, &cn) &&
	 (!dsn_a || cn->dsn == dsn_a) )
      return ( add_cid_dsn_pair(tail, cn) &&
	       PL_unify_nil(tail)
	     );

    return FALSE;
  }

  LOCK();
  for(cn=connections; cn; cn=cn->next)
  { if ( (!dsn_a || cn->dsn == dsn_a) )
    { if ( !add_cid_dsn_pair(tail, cn) )
      { UNLOCK();
	return FALSE;
      }
    }
  }
  UNLOCK();

  return PL_unify_nil(tail);
}


static foreign_t
odbc_set_connection(connection *cn, term_t option)
{ RETCODE rc;
  UWORD opt;
  UDWORD optval;

  if ( PL_is_functor(option, FUNCTOR_auto_commit1) )
  { int val;

    if ( !get_bool_arg_ex(1, option, &val) )
      return FALSE;
    opt = SQL_AUTOCOMMIT;
    optval = (val ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF);
  } else if ( PL_is_functor(option, FUNCTOR_access_mode1) )
  { atom_t val;

    if ( !get_atom_arg_ex(1, option, &val) )
      return FALSE;
    opt = SQL_ACCESS_MODE;

    if ( val == ATOM_read )
      optval = SQL_MODE_READ_ONLY;
    else if ( val == ATOM_update )
      optval = SQL_MODE_READ_WRITE;
    else
      return domain_error(val, "access_mode");
  } else if ( PL_is_functor(option, FUNCTOR_cursor_type1) )
  { atom_t val;

    if ( !get_atom_arg_ex(1, option, &val) )
      return FALSE;
    opt = SQL_CURSOR_TYPE;

    if ( val == ATOM_dynamic )
      optval = SQL_CURSOR_DYNAMIC;
    else if ( val == ATOM_forwards_only )
      optval = SQL_CURSOR_FORWARD_ONLY;
    else if ( val == ATOM_keyset_driven )
      optval = SQL_CURSOR_KEYSET_DRIVEN;
    else if ( val == ATOM_static )
      optval = SQL_CURSOR_STATIC;
    else
      return domain_error(val, "cursor_type");
  } else if ( PL_is_functor(option, FUNCTOR_silent1) )
  { int val;

    if ( !get_bool_arg_ex(1, option, &val) )
      return FALSE;

    set(cn, CTX_SILENT);

    return TRUE;
  } else if ( PL_is_functor(option, FUNCTOR_encoding1) )
  { IOENC val;

    if ( !get_encoding_arg_ex(1, option, &val) )
      return FALSE;

    cn->encoding = val;
    cn->rep_flag = enc_to_rep(val);

    return TRUE;
  } else if ( PL_is_functor(option, FUNCTOR_null1) )
  { term_t a = PL_new_term_ref();

    _PL_get_arg(1, option, a);
    cn->null = nulldef_spec(a);

    return TRUE;
  } else if ( PL_is_functor(option, FUNCTOR_wide_column_threshold1) )
  { int val;

    if ( !get_int_arg_ex(1, option, &val) )
      return FALSE;
    DEBUG(2, Sdprintf("Using wide_column_threshold = %d\n", val));
    cn->max_nogetdata = val;

    return TRUE;
  } else
    return domain_error(option, "odbc_option");

  if ( (rc=SQLSetConnectOption(cn->hdbc, opt, optval)) != SQL_SUCCESS )
    return odbc_report(henv, cn->hdbc, NULL, rc);

  return TRUE;
}


static foreign_t
pl_odbc_set_connection(term_t con, term_t option)
{ connection *cn;

  if ( !get_connection(con, &cn) )
    return FALSE;

  return odbc_set_connection(cn, option);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Options for SQLGetInfo() from http://msdn.microsoft.com/library/default.asp?url=/library/en-us/odbcsql/od_odbc_c_9qp1.asp
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef struct
{ const char *name;
  UWORD id;
  enum { text, sword, ioenc } type;
  functor_t functor;
} conn_option;

static conn_option conn_option_list[] =
{ { "database_name",	   SQL_DATABASE_NAME, text },
  { "dbms_name",           SQL_DBMS_NAME, text },
  { "dbms_version",        SQL_DBMS_VER, text },
  { "driver_name",         SQL_DRIVER_NAME, text },
  { "driver_odbc_version", SQL_DRIVER_ODBC_VER, text },
  { "driver_version",      SQL_DRIVER_VER, text },
  { "active_statements",   SQL_ACTIVE_STATEMENTS, sword },
  { "encoding",		   0, ioenc },
  { NULL, 0 }
};

static foreign_t
odbc_get_connection(term_t conn, term_t option, control_t h)
{ connection *cn;
  conn_option *opt;
  functor_t f;
  term_t a, val;

  switch(PL_foreign_control(h))
  { case PL_FIRST_CALL:
      if ( !get_connection(conn, &cn) )
	return FALSE;

      opt = conn_option_list;

      if ( PL_get_functor(option, &f) )
      {	goto find;
      }	else if ( PL_is_variable(option) )
      { f = 0;
	goto find;
      } else
	return type_error(option, "odbc_option");
    case PL_REDO:
      if ( !get_connection(conn, &cn) )
	return FALSE;

      f = 0;
      opt = PL_foreign_context_address(h);

      goto find;
    case PL_PRUNED:
    default:
      return TRUE;
  }

find:
  val = PL_new_term_ref();
  a = PL_new_term_ref();
  _PL_get_arg(1, option, a);

  for(; opt->name; opt++)
  { if ( !opt->functor )
      opt->functor = PL_new_functor(PL_new_atom(opt->name), 1);

    if ( !f || opt->functor == f )
    { char buf[256];
      SWORD len;
      RETCODE rc;

      if ( opt->type == ioenc )
      { put_encoding(val, cn->encoding);
      } else
      { if ( (rc=SQLGetInfo(cn->hdbc, opt->id,
			    buf, sizeof(buf), &len)) != SQL_SUCCESS )
	{ if ( f )
	    return odbc_report(henv, cn->hdbc, NULL, rc);
	  else
	    continue;
	}

	switch( opt->type )
	{ case text:
	    PL_put_atom_nchars(val, len, buf);
	    break;
	  case sword:
	  { SQLSMALLINT *p = (SQLSMALLINT*)buf;
	    SQLSMALLINT v = *p;

	    if ( !PL_put_integer(val, v) )
	      return FALSE;
	    break;
	  }
	  default:
	    assert(0);
	  return FALSE;
	}
      }

      if ( f )
	return PL_unify(a, val);

      if ( !PL_unify_term(option,
			  PL_FUNCTOR, opt->functor,
			  PL_TERM, val) )
	return FALSE;

      if ( opt[1].name )
	PL_retry_address(opt+1);
      else
	return TRUE;
    }
  }

  if ( f )
    return domain_error(option, "odbc_option");

  return FALSE;
}


static foreign_t
odbc_end_transaction(term_t conn, term_t action)
{ connection *cn;
  RETCODE rc;
  UWORD opt;
  atom_t a;


  if ( !get_connection(conn, &cn) )
    return FALSE;

  if ( !PL_get_atom(action, &a) )
    return type_error(action, "atom");
  if ( a == ATOM_commit )
  { opt = SQL_COMMIT;
  } else if ( a == ATOM_rollback )
  { opt = SQL_ROLLBACK;
  } else
    return domain_error(action, "transaction");

  if ( (rc=SQLTransact(henv, cn->hdbc, opt)) != SQL_SUCCESS )
    return odbc_report(henv, cn->hdbc, NULL, rc);

  return TRUE;
}


		 /*******************************
		 *	CONTEXT (STATEMENTS)	*
		 *******************************/

static context** executing_contexts = NULL;
static int executing_context_size = 0;

static context *
new_context(connection *cn)
{ context *ctxt = odbc_malloc(sizeof(context));
  RETCODE rc;

  if ( !ctxt )
    return NULL;
  memset(ctxt, 0, sizeof(*ctxt));
  ctxt->magic = CTX_MAGIC;
  ctxt->henv  = henv;
  ctxt->connection = cn;
  ctxt->null = cn->null;
  ctxt->flags = cn->flags;
  ctxt->max_nogetdata = cn->max_nogetdata;
  if ( (rc=SQLAllocStmt(cn->hdbc, &ctxt->hstmt)) != SQL_SUCCESS )
  { odbc_report(henv, cn->hdbc, NULL, rc);
    free(ctxt);
    return NULL;
  }
  statistics.statements_created++;

  return ctxt;
}


static void
unmark_and_close_context(context *ctxt)
{ int self = PL_thread_self();

  LOCK_CONTEXTS();
  clear(ctxt, CTX_EXECUTING);
  if ( self >= 0 )
    executing_contexts[self] = NULL;
  UNLOCK_CONTEXTS();
  close_context(ctxt);
}

static void
close_context(context *ctxt)
{ clear(ctxt, CTX_INUSE);

  if ( ctxt->flags & CTX_PERSISTENT )
  { if ( ctxt->hstmt )
    { ctxt->rc = SQLFreeStmt(ctxt->hstmt, SQL_CLOSE);
      if ( ctxt->rc == SQL_ERROR )
	report_status(ctxt);
    }
  } else
    free_context(ctxt);
}


static void
free_parameters(int n, parameter *params)
{ if ( n && params )
  { parameter *p = params;
    int i;

    for (i=0; i<n; i++, p++)
    { if ( p->ptr_value &&
	   p->ptr_value != (SQLPOINTER)p->buf &&
	   p->len_value != SQL_LEN_DATA_AT_EXEC(0) ) /* Using SQLPutData() */
	free(p->ptr_value);
      if ( p->source.table )
	PL_unregister_atom(p->source.table);
      if ( p->source.column )
	PL_unregister_atom(p->source.column);
    }

    free(params);
  }
}


static void
free_context(context *ctx)
{ if ( ctx->magic != CTX_MAGIC )
  { if ( ctx->magic == CTX_FREEMAGIC )
      Sdprintf("ODBC: Trying to free context twice: %p\n", ctx);
    else
      Sdprintf("ODBC: Trying to free non-context: %p\n", ctx);

    return;
  }

  ctx->magic = CTX_FREEMAGIC;

  if ( ctx->hstmt )
  { ctx->rc = SQLFreeStmt(ctx->hstmt, SQL_DROP);
    if ( ctx->rc == SQL_ERROR )
      report_status(ctx);
  }

  free_parameters(ctx->NumCols,   ctx->result);
  free_parameters(ctx->NumParams, ctx->params);
  if ( ison(ctx, CTX_SQLMALLOCED) )
    PL_free(ctx->sqltext.a);
  if ( ison(ctx, CTX_OWNNULL) )
    free_nulldef(ctx->null);
  if ( ctx->findall )
    free_findall(ctx->findall);
  free(ctx);

  statistics.statements_freed++;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
clone_context()

Create a clone of a context, so we   can have the same statement running
multiple times. Is there really no better   way  to handle this? Can't I
have multiple cursors on one statement?
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static context *
clone_context(context *in)
{ context *new;
  size_t bytes = (in->sqllen+1)*in->char_width;

  if ( !(new = new_context(in->connection)) )
    return NULL;
					/* Copy SQL statement */
  if ( !(new->sqltext.a = PL_malloc(bytes)) )
    return NULL;
  new->sqllen = in->sqllen;
  new->char_width = in->char_width;
  memcpy(new->sqltext.a, in->sqltext.a, bytes);
  set(new, CTX_SQLMALLOCED);

					/* Prepare the statement */
  if ( new->char_width == 1 )
  { TRY(new,
	SQLPrepareA(new->hstmt, new->sqltext.a, new->sqllen),
	close_context(new));
  } else
  { TRY(new,
	SQLPrepareW(new->hstmt, new->sqltext.w, new->sqllen),
	close_context(new));
  }

					/* Copy parameter declarations */
  if ( (new->NumParams = in->NumParams) > 0 )
  { int pn;
    parameter *p;

    if ( !(new->params = odbc_malloc(sizeof(parameter)*new->NumParams)) )
      return NULL;
    memcpy(new->params, in->params, sizeof(parameter)*new->NumParams);

    for(p=new->params, pn=1; pn<=new->NumParams; pn++, p++)
    { SQLLEN *vlenptr = NULL;

      switch(p->cTypeID)
      { case SQL_C_CHAR:
	case SQL_C_WCHAR:
	case SQL_C_BINARY:
	  /* if p->length_ind == 0 then we are using SQLPutData
	     and must not overwrite the index stored in ptr_value with
	     a buffer of 0 length!
	  */
	  if ( p->length_ind != 0 && !(p->ptr_value = odbc_malloc(p->length_ind+1)) )
	    return NULL;
	  vlenptr = &p->len_value;
	  break;
	case SQL_C_DATE:
	case SQL_C_TYPE_DATE:
	case SQL_C_TIME:
	case SQL_C_TYPE_TIME:
	case SQL_C_TIMESTAMP:
	  if ( !(p->ptr_value = odbc_malloc(p->len_value)) )
	    return NULL;
	  break;
	case SQL_C_SLONG:
	case SQL_C_SBIGINT:
	case SQL_C_DOUBLE:
	  p->ptr_value = (SQLPOINTER)p->buf;
	  break;
      }

      TRY(new, SQLBindParameter(new->hstmt,		/* hstmt */
				(SWORD)pn,			/* ipar */
				SQL_PARAM_INPUT,	/* fParamType */
				p->cTypeID,		/* fCType */
				p->sqlTypeID,		/* fSqlType */
				p->length_ind,		/* cbColDef */
				p->scale,		/* ibScale */
				p->ptr_value,		/* rgbValue */
				0,			/* cbValueMax */
				vlenptr),		/* pcbValue */
	  close_context(new));
    }
  }

					/* Copy result columns */
  new->db_row = in->db_row;		/* the row/N functor */

  if ( in->result )
  { new->NumCols = in->NumCols;
    if ( !(new->result  = odbc_malloc(in->NumCols*sizeof(parameter))) )
      return NULL;
    memcpy(new->result, in->result, in->NumCols*sizeof(parameter));

    if ( ison(in, CTX_BOUND) )
    { parameter *p = new->result;
      int i;

      for(i = 1; i <= new->NumCols; i++, p++)
      { if ( p->len_value > PARAM_BUFSIZE )
	{ if ( !(p->ptr_value = odbc_malloc(p->len_value)) )
	    return NULL;
	} else
	  p->ptr_value = (SQLPOINTER)p->buf;

	TRY(new, SQLBindCol(new->hstmt, (SWORD)i,
			    p->cTypeID,
			    p->ptr_value,
			    p->len_value,
			    &p->length_ind),
	    close_context(new));
      }

      set(new, CTX_BOUND);
    }
  }

  new->null    = clone_nulldef(in->null);
  new->findall = clone_findall(in->findall);

  return new;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The string is malloced by Prolog and   this probably poses problems when
using on Windows, where each DLL  has   its  own memory pool. SWI-Prolog
5.0.9 introduces PL_malloc(), PL_realloc()  and   PL_free()  for foreign
code to synchronise this problem.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
get_sql_text(context *ctxt, term_t tquery)
{ if ( PL_is_functor(tquery, FUNCTOR_minus2) )
  { if ( !formatted_string(ctxt, tquery) )
      return FALSE;
  } else
  { size_t qlen;

    if ( ctxt->connection->encoding == ENC_SQLWCHAR )
    { wchar_t *ws;

#if SIZEOF_SQLWCHAR != SIZEOF_WCHAR_T
      if ( PL_get_wchars(tquery, &qlen, &ws, CVT_ATOM|CVT_STRING))
      { wchar_t *es = ws+qlen;
	SQLWCHAR *o, *q;

	q = PL_malloc((qlen+1)*sizeof(SQLWCHAR));
	for(o=q; ws<es;)
	  *o++ = *ws++;
	*o = 0;
	ctxt->sqltext.w = q;
#else
      if ( PL_get_wchars(tquery, &qlen, &ws, CVT_ATOM|CVT_STRING|BUF_MALLOC))
      { ctxt->sqltext.w = (SQLWCHAR *)ws;
#endif
	set(ctxt, CTX_SQLMALLOCED);
	ctxt->sqllen = (SQLINTEGER)qlen;
	ctxt->char_width = sizeof(SQLWCHAR);
      } else
	return type_error(tquery, "atom_or_format");
    } else
    { char *s;
      int rep = ctxt->connection->rep_flag;

      if ( PL_get_nchars(tquery, &qlen, &s, CVT_ATOM|CVT_STRING|BUF_MALLOC|rep))
      { ctxt->sqltext.a = (unsigned char*)s;
	ctxt->sqllen = (SQLINTEGER)qlen;
	ctxt->char_width = sizeof(char);
	set(ctxt, CTX_SQLMALLOCED);
      } else
	return type_error(tquery, "atom_or_format");
    }
  }

  return TRUE;
}


static int
max_qualifier_length(connection *cn)
{ if ( isoff(cn, CTX_GOT_QLEN) )
  { SQLUSMALLINT len;
    SWORD plen;
    RETCODE rc;

    if ( (rc=SQLGetInfo(cn->hdbc, SQL_MAX_QUALIFIER_NAME_LEN,
			&len, sizeof(len), &plen)) == SQL_SUCCESS )
    { /*Sdprintf("SQL_MAX_QUALIFIER_NAME_LEN = %d\n", (int)len);*/
      cn->max_qualifier_length = (int)len; /* 0: unknown */
    } else
    { odbc_report(henv, cn->hdbc, NULL, rc);
      cn->max_qualifier_length = -1;
    }

    set(cn, CTX_GOT_QLEN);
  }

  return cn->max_qualifier_length;
}


static int
prepare_result(context *ctxt)
{ SQLSMALLINT i;
  SQLCHAR nameBuffer[NameBufferLength];
  SQLSMALLINT nameLength, dataType, decimalDigits, nullable;
  SQLULEN columnSize;			/* was SQLUINTEGER */
  parameter *ptr_result;
  SQLSMALLINT ncol;

  SQLNumResultCols(ctxt->hstmt, &ncol);
  if ( ncol == 0 )
    return TRUE;			/* no results */

  if ( ctxt->result )			/* specified types */
  { if ( ncol != ctxt->NumCols )
      return PL_warning("# columns mismatch"); /* TBD: exception */
  } else
  { ctxt->NumCols = ncol;
    ctxt->db_row = PL_new_functor(ATOM_row, ctxt->NumCols);
    if ( !(ctxt->result = odbc_malloc(sizeof(parameter)*ctxt->NumCols)) )
      return FALSE;
    memset(ctxt->result, 0, sizeof(parameter)*ctxt->NumCols);
  }

  ptr_result = ctxt->result;
  for(i = 1; i <= ctxt->NumCols; i++, ptr_result++)
  { SQLDescribeCol(ctxt->hstmt, i,
		   nameBuffer, NameBufferLength, &nameLength,
		   &dataType, &columnSize, &decimalDigits,
		   &nullable);

    if ( ison(ctxt, CTX_SOURCE) )
    { SQLLEN ival;			/* was DWORD */

      ptr_result->source.column = PL_new_atom_nchars(nameLength,
						     (char*)nameBuffer);
      if ( (ctxt->rc=SQLColAttributes(ctxt->hstmt, i,
				      SQL_COLUMN_TABLE_NAME,
				      nameBuffer,
				      NameBufferLength, &nameLength,
				      &ival)) == SQL_SUCCESS )
      { ptr_result->source.table = PL_new_atom_nchars(nameLength,
						      (char*)nameBuffer);
      } else
      { if ( !report_status(ctxt) )		/* TBD: May close ctxt */
	  return FALSE;
	ptr_result->source.table = ATOM_;
	PL_register_atom(ATOM_);
      }
    }

    ptr_result->sqlTypeID = dataType;
    ptr_result->cTypeID = CvtSqlToCType(ctxt, dataType, ptr_result->plTypeID);
    if (ptr_result->cTypeID == CVNERR)
    { free_context(ctxt);
      return PL_warning("odbc_query/2: column type not managed");
    }

    DEBUG(1, Sdprintf("prepare_result(): column %d, "
		      "sqlTypeID = %d (%s), cTypeID = %d (%s), "
		      "columnSize = %zu\n",
		      i,
		      ptr_result->sqlTypeID, sql_type_name(ptr_result->sqlTypeID),
		      ptr_result->cTypeID, sql_c_type_name(ptr_result->cTypeID),
		      (size_t)columnSize));

    if ( ison(ctxt, CTX_TABLES) )
    { switch (ptr_result->sqlTypeID)
      { case SQL_LONGVARCHAR:
	case SQL_VARCHAR:
	{ int qlen = max_qualifier_length(ctxt->connection);

	  if ( qlen > 0 )
	  { /*Sdprintf("Using SQL_MAX_QUALIFIER_NAME_LEN = %d\n", qlen);*/
	    ptr_result->len_value = qlen+1; /* play safe */
	    goto bind;
	  } else if ( qlen < 0 )	/* error getting it */
	    return FALSE;
	}
      }
    }

    switch (ptr_result->sqlTypeID)
    { case SQL_LONGVARCHAR:
      case SQL_LONGVARBINARY:
      { if ( columnSize > ctxt->max_nogetdata || columnSize == 0 )
	{ use_sql_get_data:
	  DEBUG(2,
		Sdprintf("Wide SQL_LONGVAR* column %d: using SQLGetData()\n", i));
	  ptr_result->ptr_value = NULL;	/* handle using SQLGetData() */
	  continue;
	}
	ptr_result->len_value = sizeof(char)*(columnSize+1);
	goto bind;
      }
    }

    switch (ptr_result->cTypeID)
    { case SQL_C_CHAR:
	if ( columnSize == 0 )
	  goto use_sql_get_data;
	columnSize += 2;		/* decimal dot and '-' sign */
	/*FALLTHROUGH*/
      case SQL_C_BINARY:
	if ( columnSize > ctxt->max_nogetdata || columnSize == 0 )
	  goto use_sql_get_data;
	ptr_result->len_value = sizeof(char)*(columnSize+1)*((ctxt->connection->encoding == ENC_UTF8)?4:1);
	break;
      case SQL_C_WCHAR:
	if ( columnSize > ctxt->max_nogetdata || columnSize == 0 )
	  goto use_sql_get_data;
	ptr_result->len_value = sizeof(wchar_t)*(columnSize+1);
	break;
      case SQL_C_SLONG:
	ptr_result->len_value = sizeof(SQLINTEGER);
	break;
      case SQL_C_SBIGINT:
	ptr_result->len_value = sizeof(SQLBIGINT);
	break;
      case SQL_C_DOUBLE:
	ptr_result->len_value = sizeof(SQLDOUBLE);
	break;
      case SQL_C_TYPE_DATE:
	ptr_result->len_value = sizeof(DATE_STRUCT);
	break;
      case SQL_C_TYPE_TIME:
	ptr_result->len_value = sizeof(TIME_STRUCT);
	break;
      case SQL_C_TIMESTAMP:
	ptr_result->len_value = sizeof(SQL_TIMESTAMP_STRUCT);
	break;
      default:
	Sdprintf("Oops: %s:%d: cTypeID = %d\n",
		 __FILE__, __LINE__, ptr_result->cTypeID);
	assert(0);
	return FALSE;			/* make compiler happy */
    }

  bind:
    if ( ptr_result->len_value <= PARAM_BUFSIZE )
      ptr_result->ptr_value = (SQLPOINTER)ptr_result->buf;
    else
    { if ( !(ptr_result->ptr_value = odbc_malloc(ptr_result->len_value)) )
	return FALSE;
    }

    TRY(ctxt, SQLBindCol(ctxt->hstmt, i,
			 ptr_result->cTypeID,
			 ptr_result->ptr_value,
			 ptr_result->len_value,
			 &ptr_result->length_ind),
       (void)0);
  }

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
odbc_row()  is  the  final  call  from  the  various  query  predicates,
returning a result row or, in case  of findall, the whole result-set. It
must call close_context() if we are  done   with  the  context due to an
error or the last result.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static foreign_t
odbc_row(context *ctxt, term_t trow)
{ term_t local_trow;
  fid_t fid;

  if ( !ison(ctxt, CTX_BOUND) )
  { if ( !prepare_result(ctxt) )
    { close_context(ctxt);
      return FALSE;
    }
    set(ctxt, CTX_BOUND);
  }

  if ( !ctxt->result )			/* not a SELECT statement */
  { SQLLEN rows = 0;			/* was DWORD */
    int rval;

    if ( ctxt->rc != SQL_NO_DATA_FOUND )
      ctxt->rc = SQLRowCount(ctxt->hstmt, &rows);
    if ( ctxt->rc == SQL_SUCCESS ||
	 ctxt->rc == SQL_SUCCESS_WITH_INFO ||
	 ctxt->rc == SQL_NO_DATA_FOUND )
      rval = PL_unify_term(trow,
			   PL_FUNCTOR, FUNCTOR_affected1,
			     PL_LONG, (long)rows);
    else
      rval = TRUE;

    close_context(ctxt);

    return rval;
  }

  if ( ctxt->rc == SQL_NO_DATA_FOUND )
  { close_context(ctxt);
    return FALSE;
  }

  if ( ctxt->findall )			/* findall: return the whole set */
  { term_t tail = PL_copy_term_ref(trow);
    term_t head = PL_new_term_ref();
    term_t tmp  = PL_new_term_ref();

    for(;;)
    { ctxt->rc = SQLFetch(ctxt->hstmt);

      switch(ctxt->rc)
      { case SQL_NO_DATA_FOUND:
	  close_context(ctxt);
	  return PL_unify_nil(tail);
	case SQL_SUCCESS:
	  break;
	default:
	  if ( !report_status(ctxt) )
	  { close_context(ctxt);
	    return FALSE;
	  }
      }

      if ( !PL_unify_list(tail, head, tail) ||
	   !put_findall(ctxt, tmp) ||
	   !PL_unify(head, tmp) )
      { close_context(ctxt);
	return FALSE;
      }
    }
  }

  local_trow = PL_new_term_ref();
  fid = PL_open_foreign_frame();

  for(;;)				/* normal non-deterministic access */
  { if ( ison(ctxt, CTX_PREFETCHED) )
    { clear(ctxt, CTX_PREFETCHED);
    } else
    { TRY(ctxt, SQLFetch(ctxt->hstmt), close_context(ctxt));
      if ( ctxt->rc == SQL_NO_DATA_FOUND )
      { close_context(ctxt);
	return FALSE;
      }
    }

    if ( !pl_put_row(local_trow, ctxt) )
    { close_context(ctxt);
      return FALSE;			/* with pending exception */
    }

    if ( !PL_unify(trow, local_trow) )
    { PL_rewind_foreign_frame(fid);
      continue;
    }

					/* success! */
					/* pre-fetch to get determinism */
    ctxt->rc = SQLFetch(ctxt->hstmt);
    switch(ctxt->rc)
    { case SQL_NO_DATA_FOUND:		/* no alternative */
	close_context(ctxt);
	return TRUE;
      case SQL_SUCCESS_WITH_INFO:
	report_status(ctxt);		/* Always returns TRUE */
	/*FALLTHROUGH*/
      case SQL_SUCCESS:
	set(ctxt, CTX_PREFETCHED);
	PL_retry_address(ctxt);
      default:
	if ( !report_status(ctxt) )
	{ close_context(ctxt);
	  return FALSE;
	}
	return TRUE;
    }
  }
}


static int
set_column_types(context *ctxt, term_t option)
{ term_t tail = PL_new_term_ref();
  term_t head = PL_new_term_ref();
  parameter *p;
  int ntypes;

  if ( !PL_get_arg(1, option, tail) ||
       (ntypes = list_length(tail)) < 0 )
    return FALSE;			/* not a proper list */

  ctxt->NumCols = ntypes;
  ctxt->db_row = PL_new_functor(ATOM_row, ctxt->NumCols);
  if ( !(ctxt->result = odbc_malloc(sizeof(parameter)*ctxt->NumCols)) )
    return FALSE;
  memset(ctxt->result, 0, sizeof(parameter)*ctxt->NumCols);

  for(p = ctxt->result; PL_get_list(tail, head, tail); p++)
  { if ( !get_pltype(head, &p->plTypeID) )
      return FALSE;
  }
  if ( !PL_get_nil(tail) )
    return type_error(tail, "list");

  return TRUE;
}


static int
set_statement_options(context *ctxt, term_t options)
{ if ( !PL_get_nil(options) )
  { term_t tail = PL_copy_term_ref(options);
    term_t head = PL_new_term_ref();

    while(PL_get_list(tail, head, tail))
    { if ( PL_is_functor(head, FUNCTOR_types1) )
      { if ( !set_column_types(ctxt, head) )
	  return FALSE;
      } else if ( PL_is_functor(head, FUNCTOR_null1) )
      { term_t arg = PL_new_term_ref();

	_PL_get_arg(1, head, arg);
	ctxt->null = nulldef_spec(arg);
	set(ctxt, CTX_OWNNULL);
      } else if ( PL_is_functor(head, FUNCTOR_source1) )
      { int val;

	if ( !get_bool_arg_ex(1, head, &val) )
	  return FALSE;

	if ( val )
	  set(ctxt, CTX_SOURCE);
      } else if ( PL_is_functor(head, FUNCTOR_findall2) )
      { if ( !(ctxt->findall = compile_findall(head, ctxt->flags)) )
	  return FALSE;
      } else if ( PL_is_functor(head, FUNCTOR_fetch1) )
      { atom_t a;

	if ( !get_atom_arg_ex(1, head, &a) )
	  return FALSE;
	if ( a == ATOM_auto )
	  clear(ctxt, CTX_NOAUTO);
	else if ( a == ATOM_fetch )
	  set(ctxt, CTX_NOAUTO);
	else
	{ term_t a = PL_new_term_ref();
	  _PL_get_arg(1, head, a);
	  return domain_error(a, "fetch");
	}
      } else if ( PL_is_functor(head, FUNCTOR_wide_column_threshold1) )
      { int val;

	if ( !get_int_arg_ex(1, head, &val) )
	  return FALSE;

	ctxt->max_nogetdata = val;
      } else
	return domain_error(head, "odbc_option");
    }
    if ( !PL_get_nil(tail) )
      return type_error(tail, "list");
  }

  return TRUE;
}


/* This is not thread-safe: You must hold the lock when entering! */
static int
mark_context_as_executing(int self, context* ctxt)
{ if ( self >= executing_context_size )
  { int old_size = executing_context_size;
    int i;

    executing_context_size = 16;
    while (self >= executing_context_size)
      executing_context_size <<= 1;

    if ( executing_contexts == NULL )
    { executing_contexts = odbc_malloc(executing_context_size * sizeof(context*));
      if ( executing_contexts == NULL )
	return FALSE;
    } else
    { context** tmp = odbc_realloc(executing_contexts,
				   executing_context_size * sizeof(context*));
      if ( tmp == NULL )
	return FALSE;
      executing_contexts = tmp;
    }
    for (i = old_size; i < executing_context_size; i++)
      executing_contexts[i] = NULL;
  }

  if ( self >= 0 )
    executing_contexts[self] = ctxt;
  set(ctxt, CTX_EXECUTING);

  return TRUE;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
odbc_query(+Conn, +SQL, -Row)
    Execute an SQL query, returning the result-rows 1-by-1 on
    backtracking
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static foreign_t
pl_odbc_query(term_t conn, term_t tquery, term_t trow, term_t options,
	      control_t handle)
{ context *ctxt;

  switch( PL_foreign_control(handle) )
  { case PL_FIRST_CALL:
    { connection *cn;
      int self = PL_thread_self();
      if ( !get_connection(conn, &cn) )
	return FALSE;

      if ( !(ctxt = new_context(cn)) )
	return FALSE;
      if ( !get_sql_text(ctxt, tquery) )
      { free_context(ctxt);
	return FALSE;
      }

      if ( !set_statement_options(ctxt, options) )
      { free_context(ctxt);
	return FALSE;
      }
      set(ctxt, CTX_INUSE);
      LOCK_CONTEXTS();
      if (!mark_context_as_executing(self, ctxt))
      { UNLOCK_CONTEXTS();
	return FALSE;
      }
      UNLOCK_CONTEXTS();
      if ( ctxt->char_width == 1 )
      { TRY(ctxt,
	    SQLExecDirectA(ctxt->hstmt, ctxt->sqltext.a, ctxt->sqllen),
	    unmark_and_close_context(ctxt));
      } else
      { TRY(ctxt,
	    SQLExecDirectW(ctxt->hstmt, ctxt->sqltext.w, ctxt->sqllen),
	    unmark_and_close_context(ctxt));
      }
      LOCK_CONTEXTS();
      clear(ctxt, CTX_EXECUTING);
      if ( self >= 0 )
	executing_contexts[self] = NULL;
      UNLOCK_CONTEXTS();
      return odbc_row(ctxt, trow);
    }
    case PL_REDO:
      return odbc_row(PL_foreign_context_address(handle), trow);

    default:
    case PL_PRUNED:
      free_context(PL_foreign_context_address(handle));
      return TRUE;
  }
}


		 /*******************************
		 *	DICTIONARY SUPPORT	*
		 *******************************/

static foreign_t
odbc_tables(term_t conn, term_t row, control_t handle)
{ switch( PL_foreign_control(handle) )
  { case PL_FIRST_CALL:
    { connection *cn;
      context *ctxt;

      if ( !get_connection(conn, &cn) )
	return FALSE;

      if ( !(ctxt = new_context(cn)) )
	return FALSE;
      ctxt->null = NULL;		/* use default $null$ */
      set(ctxt, CTX_TABLES);
      TRY(ctxt,
	  SQLTables(ctxt->hstmt, NULL,0,NULL,0,NULL,0,NULL,0),
	  close_context(ctxt));

      return odbc_row(ctxt, row);
    }
    case PL_REDO:
      return odbc_row(PL_foreign_context_address(handle), row);

    case PL_PRUNED:
      free_context(PL_foreign_context_address(handle));
      return TRUE;

    default:
      assert(0);
      return FALSE;
  }
}


static foreign_t
pl_odbc_column(term_t conn, term_t db, term_t row, control_t handle)
{ switch( PL_foreign_control(handle) )
  { case PL_FIRST_CALL:
    { connection *cn;
      context *ctxt;
      size_t len;
      char *s;

      if ( !get_connection(conn, &cn) )
	return FALSE;
					/* TBD: Unicode version */
      if ( !PL_get_nchars(db, &len, &s, CVT_ATOM|CVT_STRING|cn->rep_flag) )
	return type_error(db, "atom");

      if ( !(ctxt = new_context(cn)) )
	return FALSE;
      ctxt->null = NULL;		/* use default $null$ */
      set(ctxt, CTX_COLUMNS);
#ifdef SQL_SERVER_BUG
      ctxt->max_nogetdata = 8192;	/* > the 4K width column for the name */
#endif
      TRY(ctxt,
	  SQLColumns(ctxt->hstmt, NULL, 0, NULL, 0,
		     (SQLCHAR*)s, (SWORD)len, NULL, 0),
	  close_context(ctxt));

      return odbc_row(ctxt, row);
    }
    case PL_REDO:
      return odbc_row(PL_foreign_context_address(handle), row);

    case PL_PRUNED:
      free_context(PL_foreign_context_address(handle));
      return TRUE;

    default:
      assert(0);
      return FALSE;
  }
}


static foreign_t
odbc_primary_key(term_t conn, term_t table, term_t row, control_t handle)
{ switch( PL_foreign_control(handle) )
  { case PL_FIRST_CALL:
    { connection *cn;
      context *ctxt;
      size_t len;
      char *s;

      if ( !get_connection(conn, &cn) )
	return FALSE;
					/* TBD: Unicode version */
      if ( !PL_get_nchars(table, &len, &s, CVT_ATOM|CVT_STRING|cn->rep_flag) )
	return type_error(table, "atom");

      if ( !(ctxt = new_context(cn)) )
	return FALSE;
      ctxt->null = NULL;		/* use default $null$ */
      set(ctxt, CTX_PRIMARYKEY);
      TRY(ctxt,
	  SQLPrimaryKeys(ctxt->hstmt, NULL, 0, NULL, 0,
		     (SQLCHAR*)s, (SWORD)len),
	  close_context(ctxt));

      return odbc_row(ctxt, row);
    }
    case PL_REDO:
      return odbc_row(PL_foreign_context_address(handle), row);

    case PL_PRUNED:
      free_context(PL_foreign_context_address(handle));
      return TRUE;

    default:
      assert(0);
      return FALSE;
  }
}

static foreign_t
odbc_foreign_key(term_t conn, term_t pktable, term_t fktable, term_t row, control_t handle)
{ switch( PL_foreign_control(handle) )
  { case PL_FIRST_CALL:
    { connection *cn;
      context *ctxt;
      size_t lpkt = 0;
      char *spkt = 0;
      size_t lpkf = 0;
      char *spkf = 0;

      if ( !get_connection(conn, &cn) )
	return FALSE;

      int nt = 0;
      if ( PL_get_nchars(pktable, &lpkt, &spkt, CVT_ATOM|CVT_STRING|cn->rep_flag) )
	++nt;
      if ( PL_get_nchars(fktable, &lpkf, &spkf, CVT_ATOM|CVT_STRING|cn->rep_flag) )
	++nt;
      if (!nt)
	return resource_error("set at least PkTable or FkTable");

      if ( !(ctxt = new_context(cn)) )
	return FALSE;
      ctxt->null = NULL;		/* use default $null$ */
      set(ctxt, CTX_FOREIGNKEY);
      TRY(ctxt,
	  SQLForeignKeys(ctxt->hstmt, NULL, 0, NULL, 0,
		     (SQLCHAR*)spkt, (SWORD)lpkt, NULL, 0, NULL, 0, (SQLCHAR*)spkf, (SWORD)lpkf),
	  close_context(ctxt));

      return odbc_row(ctxt, row);
    }
    case PL_REDO:
      return odbc_row(PL_foreign_context_address(handle), row);

    case PL_PRUNED:
      free_context(PL_foreign_context_address(handle));
      return TRUE;

    default:
      assert(0);
      return FALSE;
  }
}


static foreign_t
odbc_types(term_t conn, term_t sqltype, term_t row, control_t handle)
{ switch( PL_foreign_control(handle) )
  { case PL_FIRST_CALL:
    { connection *cn;
      context *ctxt;
      atom_t tname;
      SWORD type;
      int v;

      if ( PL_get_integer(sqltype, &v) )
      { type = v;
      } else
      { if ( !PL_get_atom(sqltype, &tname) )
	  return type_error(sqltype, "sql_type");
	if ( tname == ATOM_all_types )
	  type = SQL_ALL_TYPES;
	else if ( !get_sqltype_from_atom(tname, &type) )
	  return domain_error(sqltype, "sql_type");
      }

      if ( !get_connection(conn, &cn) )
	return FALSE;
      if ( !(ctxt = new_context(cn)) )
	return FALSE;
      ctxt->null = NULL;		/* use default $null$ */
      TRY(ctxt,
	  SQLGetTypeInfo(ctxt->hstmt, type),
	  close_context(ctxt));

      return odbc_row(ctxt, row);
    }
    case PL_REDO:
      return odbc_row(PL_foreign_context_address(handle), row);

    case PL_PRUNED:
      free_context(PL_foreign_context_address(handle));
      return TRUE;

    default:
      assert(0);
      return FALSE;
  }
}


static foreign_t
odbc_data_sources(term_t list)
{ UCHAR dsn[SQL_MAX_DSN_LENGTH];
  UCHAR description[1024];
  SWORD dsnlen, dlen;
  UWORD dir = SQL_FETCH_FIRST;
  RETCODE rc;
  term_t tail = PL_copy_term_ref(list);
  term_t head = PL_new_term_ref();

  LOCK();
  if ( !henv )
  { SQLAllocEnv(&henv);		/* Allocate an environment handle */
    SQLSetEnvAttr(henv,
		  SQL_ATTR_ODBC_VERSION,
		  (SQLPOINTER) SQL_OV_ODBC3,
		  0);
  }
  UNLOCK();

  for(;; dir=SQL_FETCH_NEXT)
  { rc = SQLDataSources(henv,
			dir,
			dsn, sizeof(dsn)-1, &dsnlen,
			description, sizeof(description)-1, &dlen);
    switch(rc)
    { case SQL_SUCCESS:
      { if ( PL_unify_list(tail, head, tail) &&
	     PL_unify_term(head, PL_FUNCTOR, FUNCTOR_data_source2,
				   PL_NCHARS, (size_t)dsnlen, dsn,
				   PL_NCHARS, (size_t)dlen, description) )
	  continue;

	return FALSE;
      }
      case SQL_NO_DATA_FOUND:
	return PL_unify_nil(tail);
      default:
	odbc_report(henv, NULL, NULL, rc);
	return FALSE;
    }
  }
}


		 /*******************************
		 *	COMPILE STATEMENTS	*
		 *******************************/

static int
unifyStmt(term_t id, context *ctxt)
{ return PL_unify_term(id, PL_FUNCTOR, FUNCTOR_odbc_statement1,
			     PL_POINTER, ctxt);
}


static int
getStmt(term_t id, context **ctxt)
{ if ( PL_is_functor(id, FUNCTOR_odbc_statement1) )
  { term_t a = PL_new_term_ref();
    void *ptr;

    _PL_get_arg(1, id, a);
    if ( PL_get_pointer(a, &ptr) )
    { *ctxt = ptr;

      if ( (*ctxt)->magic != CTX_MAGIC )
	return existence_error(id, "odbc_statement_handle");

      return TRUE;
    }
  }

  return type_error(id, "odbc_statement_handle");
}


typedef struct
{ SWORD  type;				/* SQL_* */
  const char *text;			/* same as text */
  atom_t name;				/* Prolog name */
} sqltypedef;

static sqltypedef sqltypes[] =
{ { SQL_BIGINT,	       "bigint" },
  { SQL_BINARY,	       "binary" },
  { SQL_BIT,	       "bit" },
  { SQL_CHAR,	       "char" },
  { SQL_DATE,	       "date" },
  { SQL_DECIMAL,       "decimal" },
  { SQL_DOUBLE,	       "double" },
  { SQL_FLOAT,	       "float" },
  { SQL_INTEGER,       "integer" },
  { SQL_LONGVARBINARY, "longvarbinary" },
  { SQL_LONGVARCHAR,   "longvarchar" },
  { SQL_NUMERIC,       "numeric" },
  { SQL_REAL,	       "real" },
  { SQL_SMALLINT,      "smallint" },
  { SQL_TIME,	       "time" },
  { SQL_TIMESTAMP,     "timestamp" },
  { SQL_TINYINT,       "tinyint" },
  { SQL_VARBINARY,     "varbinary" },
  { SQL_VARCHAR,       "varchar" },
  { SQL_WCHAR,         "nchar" },
  { SQL_WLONGVARCHAR,  "longnvarchar" },
  { SQL_WVARCHAR,      "nvarchar" },
  { 0,		       NULL }
};


static SWORD
get_sqltype_from_atom(atom_t name, SWORD *type)
{ sqltypedef *def;

  for(def=sqltypes; def->text; def++)
  { if ( !def->name )
      def->name = PL_new_atom(def->text);
    if ( def->name == name )
    { *type = def->type;
      return TRUE;
    }
  }

  return FALSE;
}

static const char*
sql_type_name(SWORD type)
{ sqltypedef *def;

  for(def=sqltypes; def->text; def++)
  { if ( def->type == type )
      return (const char*)def->text;
  }

  return "?";
}


static sqltypedef pltypes[] =
{ { SQL_PL_DEFAULT,    "default" },
  { SQL_PL_ATOM,       "atom" },
  { SQL_PL_STRING,     "string" },
  { SQL_PL_CODES,      "codes" },
  { SQL_PL_INTEGER,    "integer" },
  { SQL_PL_FLOAT,      "float" },
  { SQL_PL_TIME,       "time" },
  { SQL_PL_DATE,       "date" },
  { SQL_PL_TIMESTAMP,  "timestamp" },
  { 0,		       NULL }
};


static int
get_pltype(term_t t, SWORD *type)
{ atom_t name;

  if ( PL_get_atom(t, &name) )
  { sqltypedef *def;

    for(def=pltypes; def->text; def++)
    { if ( !def->name )
	def->name = PL_new_atom(def->text);

      if ( def->name == name )
      { *type = def->type;
	return TRUE;
      }
    }

    return domain_error(t, "sql_prolog_type");
  }

  return type_error(t, "atom");
}


static const char*
pl_type_name(SWORD type)
{ sqltypedef *def;

  for(def=pltypes; def->text; def++)
  { if ( def->type == type )
      return (const char*)def->text;
  }

  return "?";
}


static sqltypedef sql_c_types[] =
{ { SQL_C_WCHAR,     "wchar" },
  { SQL_C_CHAR,	     "char" },
  { SQL_C_BINARY,    "binary" },
  { SQL_C_SLONG,     "slong" },
  { SQL_C_SBIGINT,   "sbigint" },
  { SQL_C_DOUBLE,    "double" },
  { SQL_C_DATE,	     "date" },
  { SQL_C_TYPE_DATE, "type_date" },
  { SQL_C_TIME,	     "time" },
  { SQL_C_TYPE_TIME, "type_time" },
  { SQL_C_TIMESTAMP, "timestamp" },
  { 0,		     NULL }
};


static const char*
sql_c_type_name(SWORD type)
{ sqltypedef *def;

  for(def=sql_c_types; def->text; def++)
  { if ( def->type == type )
      return (const char*)def->text;
  }

  return "?";
}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Declare parameters for prepared statements.

odbc_prepare(DSN, 'select * from product where price < ?',
	     [ integer
	     ],
	     Qid,
	     Options).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
declare_parameters(context *ctxt, term_t parms)
{ int nparams;
  term_t tail = PL_copy_term_ref(parms);
  term_t head = PL_new_term_ref();
  parameter *params;
  SWORD npar;
  SWORD pn;
  int character_size = ((ctxt->connection->encoding == ENC_UTF8)?4:1)*sizeof(char);

  TRY(ctxt,
      SQLNumParams(ctxt->hstmt, &npar),
      (void)0);
  if ( (nparams=list_length(parms)) < 0 )
    return FALSE;
  if ( npar != nparams )
    return domain_error(parms, "length"); /* TBD: What error to raise?? */

  ctxt->NumParams = nparams;
  if ( nparams == 0 )
    return TRUE;			/* no parameters */

  if ( !(ctxt->params = odbc_malloc(sizeof(parameter)*nparams)) )
    return FALSE;
  memset(ctxt->params, 0, sizeof(parameter)*nparams);
  params = ctxt->params;

  for(params = ctxt->params, pn = 1;
      PL_get_list(tail, head, tail);
      params++, pn++)
  { atom_t name;
    size_t arity;
    SWORD sqlType, fNullable;
    SQLULEN cbColDef = 0;
    SWORD plType = SQL_PL_DEFAULT;
    SQLLEN *vlenptr = NULL;		/* pointer to length */
    int isvar;

    if ( (isvar=PL_is_variable(head)) )
    { name = ATOM_default;
      arity = 0;
    } else
    { if ( PL_is_functor(head, FUNCTOR_gt2) )
      { term_t a = PL_new_term_ref();

	_PL_get_arg(1, head, a);
	if ( !get_pltype(a, &plType) )
	  return FALSE;

	_PL_get_arg(2, head, head);
      }

      if ( !PL_get_name_arity(head, &name, &arity) )
	return type_error(head, "parameter_type");
    }

    if ( name != ATOM_default )
    { int val;

      if ( !get_sqltype_from_atom(name, &sqlType) )
	return domain_error(head, "parameter_type");

					/* char(N) --> cbColDef */
      if ( get_int_arg(1, head, &val) )	/* TBD: incomplete */
	cbColDef = val;
      if ( get_int_arg(2, head, &val) )	/* decimal(cbColDef, scale) */
	params->scale = val;
    } else
    { TRY(ctxt, SQLDescribeParam(ctxt->hstmt,	/* hstmt */
				 pn,		/* ipar */
				 &sqlType,
				 &cbColDef,     /* Characters - not bytes */
				 &params->scale,
				 &fNullable),
	  (void)0);
    }

    params->sqlTypeID = sqlType;
    params->plTypeID  = plType;
    params->cTypeID   = CvtSqlToCType(ctxt, params->sqlTypeID, plType);
    params->ptr_value = (SQLPOINTER)params->buf;
    if ( isvar &&
	 !PL_unify_term(head, PL_FUNCTOR, FUNCTOR_gt2,
				PL_CHARS, pl_type_name(plType),
				PL_CHARS, sql_type_name(sqlType)) )
      return FALSE;

    DEBUG(1, Sdprintf("param: SQL:%s, Prolog:%s, C:%s, cbColDef=%d, scale=%d\n",
		      sql_type_name(sqlType),
		      pl_type_name(plType),
		      sql_c_type_name(params->cTypeID),
		      (int)cbColDef, (int)params->scale));

    switch(params->cTypeID)
    { case SQL_C_WCHAR:
	character_size = sizeof(SQLWCHAR);
	/* FALLTHROUGH */
      case SQL_C_CHAR:
      case SQL_C_BINARY:
	if ( cbColDef > 0 )
	{ if ( params->sqlTypeID == SQL_DECIMAL ||
	       params->sqlTypeID == SQL_NUMERIC )
	  { cbColDef += 2*character_size;	/* decimal dot and '-' sign */
	  }
	  if ( (cbColDef+1) * character_size > PARAM_BUFSIZE )
	  { if ( !(params->ptr_value = odbc_malloc((cbColDef+1)*character_size)) )
	      return FALSE;
	  }
	  params->length_ind = cbColDef * character_size;
	} else				/* unknown, use SQLPutData() */
	{ params->ptr_value = (SQLPOINTER)(intptr_t)pn;
	  params->len_value = SQL_LEN_DATA_AT_EXEC(0);
	  DEBUG(2, Sdprintf("Using SQLPutData() for column %d\n", pn));
	}
	vlenptr = &params->len_value;
	break;
      case SQL_C_SLONG:
	params->len_value = sizeof(SQLINTEGER);
	vlenptr = &params->len_value;
	break;
      case SQL_C_SBIGINT:
	params->len_value = sizeof(SQLBIGINT);
	vlenptr = &params->len_value;
	break;
      case SQL_C_DOUBLE:
	params->len_value = sizeof(SQLDOUBLE);
	vlenptr = &params->len_value;
	break;
      case SQL_C_DATE:
      case SQL_C_TYPE_DATE:
	if ( !(params->ptr_value = odbc_malloc(sizeof(DATE_STRUCT))) )
	  return FALSE;
	params->len_value = sizeof(DATE_STRUCT);
	vlenptr = &params->len_value;
	break;
      case SQL_C_TIME:
      case SQL_C_TYPE_TIME:
	if ( !(params->ptr_value = odbc_malloc(sizeof(TIME_STRUCT))) )
	  return FALSE;
	params->len_value = sizeof(TIME_STRUCT);
	vlenptr = &params->len_value;
	break;
      case SQL_C_TIMESTAMP:
	if ( !(params->ptr_value = odbc_malloc(sizeof(SQL_TIMESTAMP_STRUCT))) )
	  return FALSE;
	params->len_value = sizeof(SQL_TIMESTAMP_STRUCT);
	vlenptr = &params->len_value;
	break;
      default:
	Sdprintf("declare_parameters(): cTypeID %d not supported\n",
		 params->cTypeID);
    }


    TRY(ctxt, SQLBindParameter(ctxt->hstmt,		/* hstmt */
			       (SWORD)pn,		/* ipar */
			       SQL_PARAM_INPUT,		/* fParamType */
			       params->cTypeID,		/* fCType */
			       params->sqlTypeID,	/* fSqlType */
			       params->length_ind,	/* cbColDef */
			       params->scale,		/* ibScale */
			       params->ptr_value,	/* rgbValue */
			       0,			/* cbValueMax */
			       vlenptr),		/* pcbValue */
	(void)0);
  }

  return TRUE;
}


static foreign_t
odbc_prepare(term_t conn, term_t sql, term_t parms, term_t qid, term_t options)
{ connection *cn;
  context *ctxt;

  if ( !get_connection(conn, &cn) )
    return FALSE;

  if ( !(ctxt = new_context(cn)) )
    return FALSE;
  if ( !get_sql_text(ctxt, sql) )
  { free_context(ctxt);
    return FALSE;
  }

  if ( ctxt->char_width == 1 )
  { TRY(ctxt,
	SQLPrepareA(ctxt->hstmt, ctxt->sqltext.a, ctxt->sqllen),
	close_context(ctxt));
  } else
  { TRY(ctxt,
	SQLPrepareW(ctxt->hstmt, ctxt->sqltext.w, ctxt->sqllen),
	close_context(ctxt));
  }

  if ( !declare_parameters(ctxt, parms) )
  { free_context(ctxt);
    return FALSE;
  }

  ctxt->flags |= CTX_PERSISTENT;

  if ( !set_statement_options(ctxt, options) )
  { free_context(ctxt);
    return FALSE;
  }

  return unifyStmt(qid, ctxt);
}


static foreign_t
odbc_clone_statement(term_t qid, term_t cloneqid)
{ context *ctxt, *clone;

  if ( !getStmt(qid, &ctxt) )
    return FALSE;
  if ( !(clone = clone_context(ctxt)) )
    return FALSE;

  clone->flags |= CTX_PERSISTENT;

  return unifyStmt(cloneqid, clone);
}


static foreign_t
odbc_free_statement(term_t qid)
{ context *ctxt;

  if ( !getStmt(qid, &ctxt) )
    return FALSE;

  if ( ison(ctxt, CTX_INUSE) )
    clear(ctxt, CTX_PERSISTENT);	/* oops, delay! */
  else
    free_context(ctxt);

  return TRUE;
}


static int
get_date(term_t head, DATE_STRUCT* date)
{ if ( PL_is_functor(head, FUNCTOR_date3) )
  { int v;

    if ( !get_int_arg(1, head, &v) ) return FALSE;
    date->year = v;
    if ( !get_int_arg(2, head, &v) ) return FALSE;
    date->month = v;
    if ( !get_int_arg(3, head, &v) ) return FALSE;
    date->day = v;

    return TRUE;
  }

  return FALSE;
}


static int
get_time(term_t head, TIME_STRUCT* time)
{ if ( PL_is_functor(head, FUNCTOR_time3) )
  { int v;

    if ( !get_int_arg(1, head, &v) ) return FALSE;
    time->hour = v;
    if ( !get_int_arg(2, head, &v) ) return FALSE;
    time->minute = v;
    if ( !get_int_arg(3, head, &v) ) return FALSE;
    time->second = v;

    return TRUE;
  }

  return FALSE;
}


static int
get_timestamp(term_t t, SQL_TIMESTAMP_STRUCT* stamp)
{
#if defined(HAVE_LOCALTIME) || defined(HAVE_GMTIME)
  double tf;
#endif

  if ( PL_is_functor(t, FUNCTOR_timestamp7) )
  { int v;

    if ( !get_int_arg(1, t, &v) ) return FALSE;
    stamp->year = v;
    if ( !get_int_arg(2, t, &v) ) return FALSE;
    stamp->month = v;
    if ( !get_int_arg(3, t, &v) ) return FALSE;
    stamp->day = v;
    if ( !get_int_arg(4, t, &v) ) return FALSE;
    stamp->hour = v;
    if ( !get_int_arg(5, t, &v) ) return FALSE;
    stamp->minute = v;
    if ( !get_int_arg(6, t, &v) ) return FALSE;
    stamp->second = v;
    if ( !get_int_arg(7, t, &v) ) return FALSE;
    stamp->fraction = v;

    return TRUE;
#if defined(HAVE_LOCALTIME) || defined(HAVE_GMTIME)
  } else if ( PL_get_float(t, &tf) )
  { time_t t = (time_t)tf;
    long  ns = (long)((tf - (double)t) * 1000000000.0);
#if defined(HAVE_GMTIME) && defined USE_UTC
    struct tm *tm = gmtime(&t);
#else
    struct tm *tm = localtime(&t);
#endif

    if ( fabs(tf - (double)t) > 1.0 )
      return FALSE;			/* out of range */

    stamp->year	    = tm->tm_year + 1900;
    stamp->month    = tm->tm_mon + 1;
    stamp->day	    = tm->tm_mday;
    stamp->hour	    = tm->tm_hour;
    stamp->minute   = tm->tm_min;
    stamp->second   = tm->tm_sec;
    stamp->fraction = ns;

    return TRUE;
#endif
  } else
    return FALSE;
}


static int
try_null(context *ctxt, parameter *prm, term_t val, const char *expected)
{ if ( is_sql_null(val, ctxt->null) )
  { prm->len_value = SQL_NULL_DATA;

    return TRUE;
  } else
    return type_error(val, expected);
}

static unsigned int
plTypeID_convert_flags(int plTypeID, const char** expected)
{ unsigned int flags;

  switch(plTypeID)
  { case SQL_PL_DEFAULT:
      flags = CVT_ATOM|CVT_STRING;
      *expected = "text";
      break;
    case SQL_PL_ATOM:
      flags = CVT_ATOM;
      *expected = "atom";
      break;
    case SQL_PL_STRING:
      flags = CVT_STRING;
      *expected = "string";
      break;
    case SQL_PL_CODES:
      flags = CVT_LIST;
      *expected = "code_list";
      break;
    default:
      flags = 0; /* keep compiler happy */
      assert(0);
  }
  return flags;
}


static int
get_datetime(term_t t, size_t *len, char *s)
{ SQL_TIMESTAMP_STRUCT stamp;

  if ( get_timestamp(t, &stamp) )
  { size_t l;
    char *e;

    snprintf(s, *len, "%04d-%02d-%02d %02d:%02d:%02d.%09d",
	     (int)stamp.year, (int)stamp.month, (int)stamp.day,
	     (int)stamp.hour, (int)stamp.minute, (int)stamp.second,
	     (int)stamp.fraction);
    l = strlen(s);			/* return from snprintf() is not */
    e = &s[l];
    while(e[-1] == '0')
      e--;
    *e = '\0';

    *len = e-s;
    return TRUE;			/* very portable (e.g., Windows) */
  }

  return FALSE;
}


static int
bind_parameters(context *ctxt, term_t parms)
{ term_t tail = PL_copy_term_ref(parms);
  term_t head = PL_new_term_ref();
  parameter *prm;

  for(prm = ctxt->params; PL_get_list(tail, head, tail); prm++)
  { if ( prm->len_value == SQL_LEN_DATA_AT_EXEC(0) )
    { DEBUG(2, Sdprintf("bind_parameters(): Delaying column %zd\n",
                        (size_t)(prm-ctxt->params+1)));
      prm->put_data = PL_copy_term_ref(head);
      continue;
    }

    switch(prm->cTypeID)
    { case SQL_C_SLONG:
      { int32_t val;

	if ( PL_get_integer(head, &val) )
	{ SQLINTEGER sqlval = val;
	  memcpy(prm->ptr_value, &sqlval, sizeof(SQLINTEGER));
	  prm->len_value = sizeof(SQLINTEGER);
	} else if ( !try_null(ctxt, prm, head, "32 bit integer") )
	  return FALSE;
	break;
      }
      case SQL_C_SBIGINT:
      { int64_t val;

	if ( PL_get_int64(head, &val) )
	{ SQLBIGINT sqlval = val;
	  memcpy(prm->ptr_value, &sqlval, sizeof(SQLBIGINT));
	  prm->len_value = sizeof(SQLBIGINT);
	} else if ( !try_null(ctxt, prm, head, "64 bit integer") )
	  return FALSE;
	break;
      }
      case SQL_C_DOUBLE:
	if ( PL_get_float(head, (double *)prm->ptr_value) )
	  prm->len_value = sizeof(double);
	else if ( !try_null(ctxt, prm, head, "float") )
	  return FALSE;
	break;
      case SQL_C_CHAR:
      case SQL_C_WCHAR:
      case SQL_C_BINARY:
      { SQLLEN len;
	size_t l;
	char *s;
	const char *expected = "text";
	unsigned int flags = plTypeID_convert_flags(prm->plTypeID, &expected);

					/* check for NULL */
	if ( is_sql_null(head, ctxt->null) )
	{ prm->len_value = SQL_NULL_DATA;
	  break;
	}
	if ( prm->cTypeID == SQL_C_WCHAR )
	{ wchar_t *ws;
	  size_t ls;

	  if ( !PL_get_wchars(head, &ls, &ws, flags) )
	    return type_error(head, expected);
	  len = ls*sizeof(SQLWCHAR);
	  if (  len > prm->length_ind )
	  { DEBUG(1, Sdprintf("Column-width (SQL_C_WCHAR) = %zd\n",
			      (size_t)prm->length_ind));
	    return representation_error(head, "column_width");
	  }
	  prm->len_value = len;
#if SIZEOF_SQLWCHAR == SIZEOF_WCHAR_T
	  memcpy(prm->ptr_value, ws, (ls+1)*sizeof(SQLWCHAR));
#else
	{ wchar_t *es = ws+ls;
	  SQLWCHAR *o;

	  for(o=(SQLWCHAR*)prm->ptr_value; ws<es;)
	    *o++ = *ws++;
	  *o = 0;
	}
#endif
	} else
	{ char datetime_str[128];
	  int rep = (prm->cTypeID == SQL_C_CHAR ? ctxt->connection->rep_flag
						: REP_ISO_LATIN_1);

	  l = sizeof(datetime_str);
	  s = datetime_str;
	  if ( !PL_get_nchars(head, &l, &s, flags|rep) &&
	       !get_datetime(head, &l, s) )
	    return type_error(head, expected);
	  len = l;
	  if ( len > prm->length_ind )
	  { DEBUG(1, Sdprintf("Column-width (SQL_C_CHAR) = %zd\n",
			      (size_t)prm->length_ind));
	    return representation_error(head, "column_width");
	  }
	  memcpy(prm->ptr_value, s, len+1);
	  prm->len_value = len;
	}

	break;
      }
      case SQL_C_TYPE_DATE:
      { if ( get_date(head, (DATE_STRUCT*)prm->ptr_value) )
	  prm->len_value = sizeof(DATE_STRUCT);
	else if ( !try_null(ctxt, prm, head, "date") )
	  return FALSE;
	break;
      }
      case SQL_C_TYPE_TIME:
      { if ( get_time(head, (TIME_STRUCT*)prm->ptr_value) )
	  prm->len_value = sizeof(TIME_STRUCT);
	else if ( !try_null(ctxt, prm, head, "time") )
	  return FALSE;
	break;
      }
      case SQL_C_TIMESTAMP:
      { if ( get_timestamp(head, (SQL_TIMESTAMP_STRUCT*)prm->ptr_value) )
	  prm->len_value = sizeof(SQL_TIMESTAMP_STRUCT);
	else if ( !try_null(ctxt, prm, head, "timestamp") )
	  return FALSE;
	break;
      }
      default:
	return PL_warning("Unknown parameter type: %d", prm->cTypeID);
    }
  }
  if ( !PL_get_nil(tail) )
    return type_error(tail, "list");

  return TRUE;
}

static foreign_t
odbc_execute(term_t qid, term_t args, term_t row, control_t handle)
{ switch( PL_foreign_control(handle) )
  { case PL_FIRST_CALL:
    { context *ctxt;
      int self = PL_thread_self();
      if ( !getStmt(qid, &ctxt) )
	return FALSE;
      if ( ison(ctxt, CTX_INUSE) )
      { context *clone;

	if ( ison(ctxt, CTX_NOAUTO) || !(clone = clone_context(ctxt)) )
	  return context_error(qid, "in_use", "statement");
	else
	  ctxt = clone;
      }

      if ( !bind_parameters(ctxt, args) )
	return FALSE;

      set(ctxt, CTX_INUSE);
      clear(ctxt, CTX_PREFETCHED);
      LOCK_CONTEXTS();
      if (!mark_context_as_executing(self, ctxt))
      { UNLOCK_CONTEXTS();
	return FALSE;
      }
      UNLOCK_CONTEXTS();
      ctxt->rc = SQLExecute(ctxt->hstmt);
      LOCK_CONTEXTS();
      clear(ctxt, CTX_EXECUTING);
      if ( self >= 0 )
	executing_contexts[self] = NULL;
      UNLOCK_CONTEXTS();
      while( ctxt->rc == SQL_NEED_DATA )
      { PTR token;

	if ( (ctxt->rc = SQLParamData(ctxt->hstmt, &token)) == SQL_NEED_DATA )
	{ parameter *p = &ctxt->params[(intptr_t)token - 1];
	  size_t len;
	  char *s;

	  if ( is_sql_null(p->put_data, ctxt->null) )
	  { s = NULL;
	    len = SQL_NULL_DATA;
	    SQLPutData(ctxt->hstmt, s, len);
	  } else
	  { const char *expected = "text";
	    unsigned int flags = plTypeID_convert_flags(p->plTypeID, &expected);

	    if ( p->cTypeID == SQL_C_WCHAR )
	    { wchar_t *ws;

	      if ( !PL_get_wchars(p->put_data, &len, &ws, flags) )
	      { SQLCancel(ctxt->hstmt);
		return type_error(p->put_data, expected);
	      }
#if SIZEOF_SQLWCHAR == SIZEOF_WCHAR_T
	      SQLPutData(ctxt->hstmt, ws, len*sizeof(SQLWCHAR));
#else
	    { SQLWCHAR fast[256];
	      SQLWCHAR *tmp;
	      wchar_t *es = ws+len;
	      SQLWCHAR *o;

	      if ( len+1 <= sizeof(fast)/sizeof(SQLWCHAR) )
	      { tmp = fast;
	      } else
	      { if ( !(tmp = odbc_malloc((len+1)*sizeof(SQLWCHAR))) )
		{ SQLCancel(ctxt->hstmt);
		  return FALSE;
		}
	      }

	      for(o=tmp; ws<es;)
		*o++ = *ws++;
	      *o = 0;
	      SQLPutData(ctxt->hstmt, tmp, len*sizeof(SQLWCHAR));

	      if ( tmp != fast )
		free(tmp);
	    }
#endif
	    } else
	    { int rep = (p->cTypeID == SQL_C_BINARY ? REP_ISO_LATIN_1
						    : ctxt->connection->rep_flag);

	      if ( !PL_get_nchars(p->put_data, &len, &s, flags|rep) )
	      { SQLCancel(ctxt->hstmt);
		return type_error(p->put_data, expected);
	      }
	      SQLPutData(ctxt->hstmt, s, len);
	    }
	  }
	}
      }
      if ( !report_status(ctxt) )
      { close_context(ctxt);
	return FALSE;
      }

      if ( ison(ctxt, CTX_NOAUTO) )
	return TRUE;

      return odbc_row(ctxt, row);
    }

    case PL_REDO:
      return odbc_row(PL_foreign_context_address(handle), row);

    case PL_PRUNED:
      close_context(PL_foreign_context_address(handle));
      return TRUE;

    default:
      assert(0);
      return FALSE;
  }
}


static int
get_scroll_param(term_t param, int *orientation, long *offset)
{ atom_t name;
  size_t arity;

  if ( PL_get_name_arity(param, &name, &arity) )
  { if ( name == ATOM_next && arity == 0 )
    { *orientation = SQL_FETCH_NEXT;
      *offset = 0;
      return TRUE;
    } else if ( name == ATOM_prior && arity == 0 )
    { *orientation = SQL_FETCH_PRIOR;
      *offset = 0;
      return TRUE;
    } else if ( name == ATOM_first && arity == 0 )
    { *orientation = SQL_FETCH_FIRST;
      *offset = 0;
      return TRUE;
    } else if ( name == ATOM_last && arity == 0 )
    { *orientation = SQL_FETCH_LAST;
      *offset = 0;
      return TRUE;
    } else if ( name == ATOM_absolute && arity == 1 )
    { *orientation = SQL_FETCH_ABSOLUTE;
      return get_long_arg_ex(1, param, offset);
    } else if ( name == ATOM_relative && arity == 1 )
    { *orientation = SQL_FETCH_RELATIVE;
      return get_long_arg_ex(1, param, offset);
    } else if ( name == ATOM_bookmark && arity == 1 )
    { *orientation = SQL_FETCH_BOOKMARK;
      return get_long_arg_ex(1, param, offset);
    } else
      return domain_error(param, "fetch_option");
  }

  return type_error(param, "fetch_option");
}


static foreign_t
odbc_next_result_set(term_t qid, control_t handle)
{ context *ctxt;
  int rc;
  if ( !getStmt(qid, &ctxt) )
    return FALSE;
  if ( isoff(ctxt, CTX_NOAUTO) || isoff(ctxt, CTX_INUSE) || isoff(ctxt, CTX_BOUND) )
    return permission_error("next_result_set", "statement", qid);
  rc = SQLMoreResults(ctxt->hstmt);
  /* We now need to free the buffers used to retrieve the previous result set and
     re-prepare them for the new result set
  */
  SQLFreeStmt(ctxt->hstmt, SQL_UNBIND);
  free_parameters(ctxt->NumCols, ctxt->result);
  ctxt->result = NULL;
  clear(ctxt, CTX_BOUND);

  switch (rc)
  { case SQL_NO_DATA_FOUND:
      PL_fail;
    case SQL_SUCCESS_WITH_INFO:
      report_status(ctxt);
      /*FALLTHROUGH*/
    case SQL_SUCCESS:
      PL_succeed;
    default:
      if ( !report_status(ctxt) )
      { close_context(ctxt);
	return FALSE;
      }
      return TRUE;
  }
}

static foreign_t
odbc_fetch(term_t qid, term_t row, term_t options)
{ context *ctxt;
  term_t local_trow = PL_new_term_ref();
  int orientation;
  long offset;

  if ( !getStmt(qid, &ctxt) )
    return FALSE;
  if ( isoff(ctxt, CTX_NOAUTO) || isoff(ctxt, CTX_INUSE) )
    return permission_error("fetch", "statement", qid);

  if ( !ison(ctxt, CTX_BOUND) )
  { if ( !prepare_result(ctxt) )
      return FALSE;
    set(ctxt, CTX_BOUND);
  }

  if ( !ctxt->result )			/* not a SELECT statement */
  { SQLLEN rows = 0;			/* was DWORD */

    if ( ctxt->rc != SQL_NO_DATA_FOUND )
      ctxt->rc = SQLRowCount(ctxt->hstmt, &rows);
    if ( ctxt->rc == SQL_SUCCESS ||
	 ctxt->rc == SQL_SUCCESS_WITH_INFO ||
	 ctxt->rc == SQL_NO_DATA_FOUND )
      return PL_unify_term(row,
			   PL_FUNCTOR, FUNCTOR_affected1,
			   PL_LONG, (long)rows);
    return report_status(ctxt);
  }

  if ( PL_get_nil(options) )
  { orientation = SQL_FETCH_NEXT;
  } else if ( PL_is_list(options) )
  { term_t tail = PL_copy_term_ref(options);
    term_t head = PL_new_term_ref();

    while(PL_get_list(tail, head, tail))
    { if ( !get_scroll_param(head, &orientation, &offset) )
	return FALSE;
    }
    if ( !PL_get_nil(tail) )
      return type_error(tail, "list");
  } else if ( !get_scroll_param(options, &orientation, &offset) )
    return FALSE;

  if ( orientation == SQL_FETCH_NEXT )
    ctxt->rc = SQLFetch(ctxt->hstmt);
  else
    ctxt->rc = SQLFetchScroll(ctxt->hstmt,
			      (SQLSMALLINT)orientation,
			      (SQLINTEGER)offset);

  switch(ctxt->rc)
  { case SQL_NO_DATA_FOUND:		/* no alternative */
      return PL_unify_atom(row, ATOM_end_of_file);
    case SQL_SUCCESS_WITH_INFO:
      report_status(ctxt);
      /*FALLTHROUGH*/
    case SQL_SUCCESS:
      if ( !pl_put_row(local_trow, ctxt) )
      { close_context(ctxt);
	return FALSE;			/* with pending exception */
      }

      return PL_unify(local_trow, row);
    default:
      if ( !report_status(ctxt) )
      { close_context(ctxt);
	return FALSE;
      }

      return TRUE;
  }
}


static foreign_t
odbc_close_statement(term_t qid)
{ context *ctxt;

  if ( !getStmt(qid, &ctxt) )
    return FALSE;

  close_context(ctxt);

  return TRUE;
}

#ifdef O_PLMT
static foreign_t
odbc_cancel_thread(term_t Tid)
{ int tid;

  if ( !PL_get_thread_id_ex(Tid, &tid) )
    return FALSE;

  LOCK_CONTEXTS();
  if (tid >= 0 &&
      tid < executing_context_size &&
      executing_contexts[tid] != NULL &&
      ison(executing_contexts[tid], CTX_EXECUTING))
    SQLCancel(executing_contexts[tid]->hstmt);
  UNLOCK_CONTEXTS();

  return TRUE;
}
#endif


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
$odbc_statistics/1

	NOTE: enumeration of available statistics is done in Prolog
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static functor_t FUNCTOR_statements2;	/* statements(created,freed) */

static int
unify_int_arg(int pos, term_t t, long val)
{ term_t a = PL_new_term_ref();

  if ( PL_get_arg(pos, t, a) )
    return PL_unify_integer(a, val);

  return FALSE;
}


static foreign_t
odbc_statistics(term_t what)
{ if ( !PL_is_compound(what) )
    return type_error(what, "compound");

  if ( PL_is_functor(what, FUNCTOR_statements2) )
  { if ( unify_int_arg(1, what, statistics.statements_created) &&
	 unify_int_arg(2, what, statistics.statements_freed) )
      return TRUE;
  } else
    return domain_error(what, "odbc_statistics");

  return FALSE;
}


static foreign_t
odbc_debug(term_t level)
{ if ( !PL_get_integer(level, &odbc_debuglevel) )
    return type_error(level, "integer");

  return TRUE;
}

static foreign_t
pl_odbc_set_option(term_t option)
{
  if ( PL_is_functor(option, FUNCTOR_connection_pooling1) )
  { int is_pooled = 0;
    if ( !get_bool_arg_ex(1, option, &is_pooled) )
      return FALSE;
    if (is_pooled)    /* Note that it is not possible to turn pooling off once it is turned on */
    { if ( SQLSetEnvAttr(NULL,
			 SQL_ATTR_CONNECTION_POOLING,
			 (SQLPOINTER)SQL_CP_ONE_PER_HENV,
			 SQL_IS_INTEGER) != SQL_SUCCESS)
      { return PL_warning("Could not configure connection pooling");
      }
    }
  }
  return TRUE;
}


#define MKFUNCTOR(name, arity) PL_new_functor(PL_new_atom(name), arity)
#define NDET(name, arity, func) PL_register_foreign(name, arity, func, \
						    PL_FA_NONDETERMINISTIC)
#define DET(name, arity, func) PL_register_foreign(name, arity, func, 0)

install_t
install_odbc4pl()
{  INIT_CONTEXT_LOCK();
   ATOM_row	      =	PL_new_atom("row");
   ATOM_informational =	PL_new_atom("informational");
   ATOM_default	      =	PL_new_atom("default");
   ATOM_once	      =	PL_new_atom("once");
   ATOM_multiple      =	PL_new_atom("multiple");
   ATOM_commit	      =	PL_new_atom("commit");
   ATOM_rollback      =	PL_new_atom("rollback");
   ATOM_atom	      =	PL_new_atom("atom");
   ATOM_string	      =	PL_new_atom("string");
   ATOM_codes	      =	PL_new_atom("codes");
   ATOM_integer	      =	PL_new_atom("integer");
   ATOM_float	      =	PL_new_atom("float");
   ATOM_time	      =	PL_new_atom("time");
   ATOM_date	      =	PL_new_atom("date");
   ATOM_timestamp     =	PL_new_atom("timestamp");
   ATOM_all_types     = PL_new_atom("all_types");
   ATOM_null          = PL_new_atom("$null$");
   ATOM_	      = PL_new_atom("");
   ATOM_read	      = PL_new_atom("read");
   ATOM_update	      = PL_new_atom("update");
   ATOM_dynamic	      = PL_new_atom("dynamic");
   ATOM_forwards_only = PL_new_atom("forwards_only");
   ATOM_keyset_driven = PL_new_atom("keyset_driven");
   ATOM_static	      = PL_new_atom("static");
   ATOM_auto	      = PL_new_atom("auto");
   ATOM_fetch	      = PL_new_atom("fetch");
   ATOM_end_of_file   = PL_new_atom("end_of_file");
   ATOM_next          = PL_new_atom("next");
   ATOM_prior         = PL_new_atom("prior");
   ATOM_first         = PL_new_atom("first");
   ATOM_last          = PL_new_atom("last");
   ATOM_absolute      = PL_new_atom("absolute");
   ATOM_relative      = PL_new_atom("relative");
   ATOM_bookmark      = PL_new_atom("bookmark");
   ATOM_strict        = PL_new_atom("strict");
   ATOM_relaxed       = PL_new_atom("relaxed");

   FUNCTOR_timestamp7		 = MKFUNCTOR("timestamp", 7);
   FUNCTOR_time3		 = MKFUNCTOR("time", 3);
   FUNCTOR_date3		 = MKFUNCTOR("date", 3);
   FUNCTOR_odbc3		 = MKFUNCTOR("odbc", 3);
   FUNCTOR_error2		 = MKFUNCTOR("error", 2);
   FUNCTOR_type_error2		 = MKFUNCTOR("type_error", 2);
   FUNCTOR_domain_error2	 = MKFUNCTOR("domain_error", 2);
   FUNCTOR_existence_error2	 = MKFUNCTOR("existence_error", 2);
   FUNCTOR_resource_error1	 = MKFUNCTOR("resource_error", 1);
   FUNCTOR_permission_error3	 = MKFUNCTOR("permission_error", 3);
   FUNCTOR_representation_error1 = MKFUNCTOR("representation_error", 1);
   FUNCTOR_odbc_statement1	 = MKFUNCTOR("$odbc_statement", 1);
   FUNCTOR_odbc_connection1	 = MKFUNCTOR("$odbc_connection", 1);
   FUNCTOR_encoding1		 = MKFUNCTOR("encoding", 1);
   FUNCTOR_user1		 = MKFUNCTOR("user", 1);
   FUNCTOR_password1		 = MKFUNCTOR("password", 1);
   FUNCTOR_driver_string1	 = MKFUNCTOR("driver_string", 1);
   FUNCTOR_alias1		 = MKFUNCTOR("alias", 1);
   FUNCTOR_mars1		 = MKFUNCTOR("mars", 1);
   FUNCTOR_connection_pooling1	 = MKFUNCTOR("connection_pooling", 1);
   FUNCTOR_connection_pool_mode1 = MKFUNCTOR("connection_pool_mode", 1);
   FUNCTOR_odbc_version1	 = MKFUNCTOR("odbc_version", 1);
   FUNCTOR_open1		 = MKFUNCTOR("open", 1);
   FUNCTOR_auto_commit1		 = MKFUNCTOR("auto_commit", 1);
   FUNCTOR_types1		 = MKFUNCTOR("types", 1);
   FUNCTOR_minus2		 = MKFUNCTOR("-", 2);
   FUNCTOR_gt2			 = MKFUNCTOR(">", 2);
   FUNCTOR_context_error3	 = MKFUNCTOR("context_error", 3);
   FUNCTOR_statements2		 = MKFUNCTOR("statements", 2);
   FUNCTOR_data_source2		 = MKFUNCTOR("data_source", 2);
   FUNCTOR_null1		 = MKFUNCTOR("null", 1);
   FUNCTOR_source1		 = MKFUNCTOR("source", 1);
   FUNCTOR_column3		 = MKFUNCTOR("column", 3);
   FUNCTOR_access_mode1		 = MKFUNCTOR("access_mode", 1);
   FUNCTOR_cursor_type1		 = MKFUNCTOR("cursor_type", 1);
   FUNCTOR_silent1		 = MKFUNCTOR("silent", 1);
   FUNCTOR_findall2		 = MKFUNCTOR("findall", 2);
   FUNCTOR_affected1		 = MKFUNCTOR("affected", 1);
   FUNCTOR_fetch1		 = MKFUNCTOR("fetch", 1);
   FUNCTOR_wide_column_threshold1= MKFUNCTOR("wide_column_threshold", 1);

   DET("odbc_set_option",	   1, pl_odbc_set_option);
   DET("odbc_connect",		   3, pl_odbc_connect);
   DET("odbc_disconnect",	   1, pl_odbc_disconnect);
   DET("odbc_current_connections", 3, odbc_current_connections);
   DET("odbc_set_connection",	   2, pl_odbc_set_connection);
   NDET("odbc_get_connection",	   2, odbc_get_connection);
   DET("odbc_end_transaction",	   2, odbc_end_transaction);

   DET("odbc_prepare",		   5, odbc_prepare);
   DET("odbc_clone_statement",	   2, odbc_clone_statement);
   DET("odbc_free_statement",	   1, odbc_free_statement);
   NDET("odbc_execute",		   3, odbc_execute);
   DET("odbc_fetch",		   3, odbc_fetch);
   DET("odbc_next_result_set",	   1, odbc_next_result_set);
   DET("odbc_close_statement",	   1, odbc_close_statement);
#ifdef O_PLMT
   DET("odbc_cancel_thread",	   1, odbc_cancel_thread);
#endif

   NDET("odbc_query",		   4, pl_odbc_query);
   NDET("odbc_tables",		   2, odbc_tables);
   NDET("odbc_column",		   3, pl_odbc_column);
   NDET("odbc_types",		   3, odbc_types);
   DET("odbc_data_sources",	   1, odbc_data_sources);

   DET("$odbc_statistics",	   1, odbc_statistics);
   DET("odbc_debug",		   1, odbc_debug);

   NDET("odbc_primary_key",	   3, odbc_primary_key);
   NDET("odbc_foreign_key",	   4, odbc_foreign_key);
}


install_t
uninstall_odbc()			/* TBD: make sure the library is */
{ LOCK();
  if ( henv )				/* not in use! */
  { SQLFreeEnv(henv);
    henv = NULL;
  }
  UNLOCK();
}


		 /*******************************
		 *	      TYPES		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
MS SQL Server seems to store the   dictionary  in UNICODE, returning the
types SQL_WCHAR, etc.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static SWORD
CvtSqlToCType(context *ctxt, SQLSMALLINT fSqlType, SQLSMALLINT plTypeID)
{ switch(plTypeID)
  { case SQL_PL_DEFAULT:
      switch (fSqlType)
      { case SQL_CHAR:
	case SQL_VARCHAR:
	case SQL_LONGVARCHAR:
	  return SQL_C_CHAR;
#ifdef SQL_WCHAR
	case SQL_WCHAR:			/* see note above */
	case SQL_WVARCHAR:
	case SQL_WLONGVARCHAR:
	  return ctxt->connection->encoding == ENC_SQLWCHAR ? SQL_C_WCHAR
							    : SQL_C_CHAR;
#endif

	case SQL_BINARY:
	case SQL_VARBINARY:
	case SQL_LONGVARBINARY:
	  return SQL_C_BINARY;

	case SQL_DECIMAL:
	case SQL_NUMERIC:
	  return SQL_C_CHAR;

	case SQL_REAL:
	case SQL_FLOAT:
	case SQL_DOUBLE:
	  return SQL_C_DOUBLE;

	case SQL_BIT:
	case SQL_TINYINT:
	case SQL_SMALLINT:
	case SQL_INTEGER:
	  return SQL_C_SLONG;

	case SQL_BIGINT:		/* 64-bit integers */
	  return SQL_C_SBIGINT;

	case SQL_DATE:
	case SQL_TYPE_DATE:
	  return SQL_C_TYPE_DATE;
	case SQL_TIME:
	case SQL_TYPE_TIME:
	  return SQL_C_TYPE_TIME;
	case SQL_TIMESTAMP:
	case SQL_TYPE_TIMESTAMP:
	  return SQL_C_TIMESTAMP;
	default:
	  if ( !ison(ctxt, CTX_SILENT) )
	    Sdprintf("Mapped unknown fSqlType %d to atom\n", fSqlType);
	  return SQL_C_CHAR;
      }
    case SQL_PL_ATOM:
    case SQL_PL_STRING:
    case SQL_PL_CODES:
      switch (fSqlType)
      { case SQL_BINARY:
	case SQL_VARBINARY:
	case SQL_LONGVARBINARY:
	  return SQL_C_BINARY;
	case SQL_WCHAR:
	case SQL_WVARCHAR:
	case SQL_WLONGVARCHAR:
	  return ctxt->connection->encoding == ENC_SQLWCHAR ? SQL_C_WCHAR
							    : SQL_C_CHAR;
	default:
	  return SQL_C_CHAR;
      }
    case SQL_PL_INTEGER:
      switch(fSqlType)
      { case SQL_TIMESTAMP:
	  return SQL_C_TIMESTAMP;
	case SQL_BIGINT:		/* 64-bit integers */
	  return SQL_C_SBIGINT;
	default:
	  return SQL_C_SLONG;
      }
    case SQL_PL_FLOAT:
      switch(fSqlType)
      { case SQL_TIMESTAMP:
	  return SQL_C_TIMESTAMP;
	default:
	  return SQL_C_DOUBLE;
      }
    case SQL_PL_DATE:
      return SQL_C_TYPE_DATE;
    case SQL_PL_TIME:
      return SQL_C_TYPE_TIME;
    case SQL_PL_TIMESTAMP:
      return SQL_C_TIMESTAMP;
    default:
      assert(0);
      return CVNERR;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
pl_put_column(context *c, int nth, term_t col)

    Put the nth (0-based) result column of the statement in the Prolog
    variable col.  If the source(true) option is in effect, bind col to
    column(Table, Column, Value)

    If ptr_value is NULL, prepare_result() has not used SQLBindCol() due
    to a potentionally too large field such as for SQL_LONGVARCHAR
    columns.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
plTypeID_to_pltype(int plTypeID)
{ switch( plTypeID )
  { case SQL_PL_DEFAULT:
    case SQL_PL_ATOM:
      return PL_ATOM;
    case SQL_PL_STRING:
      return PL_STRING;
    case SQL_PL_CODES:
      return PL_CODE_LIST;
    default:
      assert(0);
      return FALSE;
  }
}


WUNUSED static int
put_chars(term_t val, int plTypeID, int rep, size_t len, const char *chars)
{ int pltype = plTypeID_to_pltype(plTypeID);

  return PL_unify_chars(val, pltype|rep, len, chars);
}


WUNUSED static int
put_wchars(term_t val, int plTypeID, size_t len, const SQLWCHAR *chars)
{ int pltype = plTypeID_to_pltype(plTypeID);

#if SIZEOF_SQLWCHAR == SIZEOF_WCHAR_T
  return PL_unify_wchars(val, pltype, len, chars);
#else
  wchar_t fast[256];
  wchar_t *tmp, *o;
  const SQLWCHAR *es = &chars[len];
  int rc;

  if ( len+1 <= sizeof(fast)/sizeof(fast[0]) )
  { tmp = fast;
  } else
  { if ( !(tmp = odbc_malloc((len+1)*sizeof(wchar_t))) )
      return FALSE;
  }

  for(o=tmp; chars<es;)
    *o++ = *chars++;
  *o = 0;
  rc = PL_unify_wchars(val, pltype, len, tmp);

  if ( tmp != fast )
    free(tmp);

  return rc;
#endif
}


static int
pl_put_column(context *c, int nth, term_t col)
{ parameter *p = &c->result[nth];
  term_t cell;
  term_t val;

  if ( ison(c, CTX_SOURCE) )
  { cell = PL_new_term_refs(3);

    PL_put_atom(cell+0, p->source.table);
    PL_put_atom(cell+1, p->source.column);
    val = cell+2;
  } else
  { val = col;
    cell = 0;				/* make compiler happy */
  }

  if ( !p->ptr_value )			/* use SQLGetData() */
  { char buf[256];
    char *data = buf;
    SQLLEN len;

    DEBUG(2, Sdprintf("Fetching value for column %d using SQLGetData()\n",
		      nth+1));

    c->rc = SQLGetData(c->hstmt, (UWORD)(nth+1), p->cTypeID,
		       buf, sizeof(buf), &len);

    if ( c->rc == SQL_SUCCESS || c->rc == SQL_SUCCESS_WITH_INFO )
    { DEBUG(2, Sdprintf("Got %ld bytes\n", len));
      if ( len == SQL_NULL_DATA )
      { if ( !put_sql_null(val, c->null) )
	  return FALSE;
	goto ok;
      } else if ( len == SQL_NO_TOTAL )
      { int pad = p->cTypeID == SQL_C_CHAR ? 1 : 0;
	size_t readsofar = sizeof(buf) - pad;
	SQLLEN bufsize = 2048;			/* must be > sizeof(buf) */

	if ( !(data = odbc_malloc(bufsize)) )
	  return FALSE;
	memcpy(data, buf, sizeof(buf));

	do /* Read blocks */
	{ c->rc = SQLGetData(c->hstmt, (UWORD)(nth+1), p->cTypeID,
			     &data[readsofar], bufsize-readsofar, &len);
	  if ( c->rc == SQL_ERROR )
	  { DEBUG(1, Sdprintf("SQLGetData() returned %d\n", c->rc));
	    return report_status(c);
	  } else if ( len == SQL_NO_DATA )	/* Previous block was final one */
	  { len += readsofar;
	    goto got_all_data;
	  } else if ( len == SQL_NO_TOTAL )	/* More blocks are yet to come */
	  { size_t chunk = bufsize-readsofar-pad;
	    readsofar += chunk;
	    bufsize *= 2;
	    if ( !(data = odbc_realloc(data, bufsize)) )
	      return FALSE;
	  } else if ( (ssize_t)len <= (ssize_t)(bufsize-readsofar) )
	  { len += readsofar;			/* This block is the last one */
	    goto got_all_data;
	  } else				/* Is this possible? */
	  { readsofar+= len;			/* It is analgous to the case */
	    bufsize *= 2;			/* below where SQL_NO_TOTAL is */
	    if ( !(data = odbc_realloc(data, bufsize)) ) /* not returned */
	      return FALSE;
	  }
	} while(c->rc != SQL_SUCCESS && c->rc != SQL_NO_DATA);
	len = readsofar;
      } else if ( len >= (SDWORD)sizeof(buf) )
      { int pad;
	size_t todo;
	SQLLEN len2;
	char *ep;
	int part = 2;

	switch(p->cTypeID)
	{ case SQL_C_CHAR:
	    pad = sizeof(SQLCHAR);
	    break;
	  case SQL_C_WCHAR:
	    pad = sizeof(SQLWCHAR);
	    break;
	  default:
	    pad = 0;
	}
	todo = len-sizeof(buf)+2*pad;
	if ( !(data = odbc_malloc(len+pad)) )
	  return FALSE;
	memcpy(data, buf, sizeof(buf));	/* you don't get the data twice! */
	ep = data+sizeof(buf)-pad;

	while(todo > 0)
	{ c->rc = SQLGetData(c->hstmt, (UWORD)(nth+1), p->cTypeID,
			     ep, todo, &len2);
	  DEBUG(2, Sdprintf("Requested %zd bytes for part %d; \
			     pad=%d; got %ld\n",
			    todo, part, pad, len2));
	  todo -= len2;
	  ep += len2;
	  part++;

	  switch( c->rc )
	  { case SQL_SUCCESS:
	      len = ep-data;
	      goto got_all_data;
	    case SQL_SUCCESS_WITH_INFO:
	      break;
	    default:
	    { Sdprintf("ERROR: %d\n", c->rc);
	      free(data);
	      return report_status(c);
	    }
	  }
	}
      }
    } else
    { DEBUG(1, Sdprintf("SQLGetData() returned %d\n", c->rc));
      return report_status(c);
    }

  got_all_data:
    if ( p->cTypeID == SQL_C_WCHAR )
    { if ( !put_wchars(val, p->plTypeID, len/sizeof(SQLWCHAR), (SQLWCHAR*)data) )
      { if ( data != buf )
	  free(data);
	return FALSE;
      }
    } else
    { int rep = (p->cTypeID == SQL_C_BINARY ? REP_ISO_LATIN_1
					    : c->connection->rep_flag);

      if ( !put_chars(val, p->plTypeID, rep, len, data) )
      { if ( data != buf )
	  free(data);
	return FALSE;
      }
    }
    if ( data != buf )
      free(data);
    goto ok;
  }

  if ( p->length_ind == SQL_NULL_DATA )
  { if ( !put_sql_null(val, c->null) )
      return FALSE;
  } else
  { int rc;

    switch( p->cTypeID )
    { case SQL_C_CHAR:
	rc = put_chars(val, p->plTypeID, c->connection->rep_flag,
		       p->length_ind, (char*)p->ptr_value);
	break;
      case SQL_C_WCHAR:
	rc = put_wchars(val, p->plTypeID,
			p->length_ind/sizeof(SQLWCHAR), (SQLWCHAR*)p->ptr_value);
	break;
      case SQL_C_BINARY:
	rc = put_chars(val, p->plTypeID, REP_ISO_LATIN_1,
		       p->length_ind, (char*)p->ptr_value);
	break;
      case SQL_C_SLONG:
	rc = PL_put_integer(val,*(SQLINTEGER *)p->ptr_value);
	break;
      case SQL_C_SBIGINT:
	rc = PL_put_int64(val, *(SQLBIGINT *)p->ptr_value);
	break;
      case SQL_C_DOUBLE:
	rc = PL_put_float(val,*(SQLDOUBLE *)p->ptr_value);
	break;
      case SQL_C_TYPE_DATE:
      { DATE_STRUCT* ds = (DATE_STRUCT*)p->ptr_value;
	term_t av;

	rc = ( (av=PL_new_term_refs(3)) &&
	       PL_put_integer(av+0, ds->year) &&
	       PL_put_integer(av+1, ds->month) &&
	       PL_put_integer(av+2, ds->day) &&
	       PL_cons_functor_v(val, FUNCTOR_date3, av)
	     );
	break;
      }
      case SQL_C_TYPE_TIME:
      { TIME_STRUCT* ts = (TIME_STRUCT*)p->ptr_value;
	term_t av;

	rc = ( (av=PL_new_term_refs(3)) &&
	       PL_put_integer(av+0, ts->hour) &&
	       PL_put_integer(av+1, ts->minute) &&
	       PL_put_integer(av+2, ts->second) &&
	       PL_cons_functor_v(val, FUNCTOR_time3, av)
	     );
	break;
      }
      case SQL_C_TIMESTAMP:
      { SQL_TIMESTAMP_STRUCT* ts = (SQL_TIMESTAMP_STRUCT*)p->ptr_value;

	switch( p->plTypeID )
	{ case SQL_PL_DEFAULT:
	  case SQL_PL_TIMESTAMP:
	  { term_t av;

	    rc = ( (av=PL_new_term_refs(7)) &&
		   PL_put_integer(av+0, ts->year) &&
		   PL_put_integer(av+1, ts->month) &&
		   PL_put_integer(av+2, ts->day) &&
		   PL_put_integer(av+3, ts->hour) &&
		   PL_put_integer(av+4, ts->minute) &&
		   PL_put_integer(av+5, ts->second) &&
		   PL_put_integer(av+6, ts->fraction) &&
		   PL_cons_functor_v(val, FUNCTOR_timestamp7, av)
		 );
	    break;
	  }
	  case SQL_PL_INTEGER:
	  case SQL_PL_FLOAT:
#ifdef HAVE_TIMEGM
	  { struct tm tm;
	    time_t t;

#ifndef USE_UTC
	    t = time(NULL);
	    tm = *localtime(&t);
#else
	    memset(&tm, 0, sizeof(tm));
#endif
	    tm.tm_year  = ts->year - 1900;
	    tm.tm_mon   = ts->month-1;
	    tm.tm_mday  = ts->day;
	    tm.tm_hour  = ts->hour;
	    tm.tm_min   = ts->minute;
	    tm.tm_sec   = ts->second;

#ifdef USE_UTC
	    t = timegm(&tm);
#else
	    t = mktime(&tm);
#endif

	    if ( p->plTypeID == SQL_PL_INTEGER )
	      rc = PL_put_int64(val, t);
	    else
	      rc = PL_put_float(val, (double)t); /* TBD: fraction */
	  }
#else
	    return PL_warning("System doesn't support mktime()/timegm()");
#endif
	    break;
	  default:
	    rc = 0; /* keep compiler happy */
	    assert(0);
	}
	break;
      }
      default:
	return PL_warning("ODBC: Unknown cTypeID: %d",
			  p->cTypeID);
    }
    if ( !rc )
      return FALSE;
  }

ok:
  if ( ison(c, CTX_SOURCE) )
    return PL_cons_functor_v(col, FUNCTOR_column3, cell);

  return TRUE;
}




/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Store a row
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
pl_put_row(term_t row, context *c)
{ term_t columns = PL_new_term_refs(c->NumCols);
  SQLSMALLINT i;

  for (i=0; i<c->NumCols; i++)
  { if ( !pl_put_column(c, i, columns+i) )
      return FALSE;			/* with exception */
  }

  return PL_cons_functor_v(row, c->db_row, columns);
}


#ifdef EMULATE_TIMEGM

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
timegm() is provided by glibc and the inverse of gmtime().  The glibc
library suggests using mktime with TZ=UTC as alternative.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static time_t
timegm(struct tm *tm)
{ char *otz = getenv("TZ");
  time_t t;
  char oenv[20];

  if ( otz && strlen(otz) < 10 )	/* avoid buffer overflow */
  { putenv("TZ=UTC");
    t = mktime(tm);
    strcpy(oenv, "TZ=");
    strcat(oenv, otz);
    putenv(oenv);
  } else if ( otz )
  { Sdprintf("Too long value for TZ: %s", otz);
    t = mktime(tm);
  } else				/* not set, what to do? */
  { t = mktime(tm);
  }

  return t;
}


#endif
