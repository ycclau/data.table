#include "data.table.h"
#include <R.h>
#include <errno.h>
#include <Rinternals.h>
#include <unistd.h>  // for access()
#include <fcntl.h>
#include <time.h>
#include <omp.h>
#ifdef WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#define WRITE _write
#define CLOSE _close
#else
#define WRITE write
#define CLOSE close
#endif

static inline int maxStrLen(SEXP x, int na_len) {
  // The max nchar of any string in a column or factor level
  int max=na_len, nrow=length(x);
  for (int i=0; i<nrow; i++) {
    int l = LENGTH(STRING_ELT(x,i));
    if (l>max) max=l;
  }
  // TODO if(quote) count them here (we hopped already to get LENGTH), add that quote count to max
  //      exactly and remove the *(1+quote) later
  //      if there are no quotes present in any string in a column save in quotePresent[ncol] lookup
  //      and switch to memcpy when it's known no quotes are present to be escaped for that column
  return max;
}

#define QUOTE_FIELD \
  *ch++ = QUOTE; \
  for (const char *ch2 = CHAR(str); *ch2 != '\0'; ch2++) { \
    if (*ch2 == QUOTE) *ch++ = ESCAPE_QUOTE; \
    if (qmethod_escape && *ch2 == ESCAPE) *ch++ = ESCAPE; \
    *ch++ = *ch2; \
  } \
  *ch++ = QUOTE

SEXP writefile(SEXP list_of_columns,
               SEXP filenameArg,
               SEXP col_sep_Arg,
               SEXP row_sep_Arg,
               SEXP na_Arg,
               SEXP quoteArg,           // TRUE|FALSE
               SEXP qmethod_escapeArg,  // TRUE|FALSE
               SEXP append,             // TRUE|FALSE
               SEXP col_names,          // TRUE|FALSE
               SEXP verboseArg)
{
  if (!isNewList(list_of_columns)) error("fwrite must be passed an object of type list, data.table or data.frame");
  RLEN ncols = length(list_of_columns);
  if (ncols==0) error("fwrite must be passed a non-empty list");
  RLEN nrows = length(VECTOR_ELT(list_of_columns, 0));
  for (int i=1; i<ncols; i++) {
    if (nrows != length(VECTOR_ELT(list_of_columns, i)))
      error("Column %d's length (%d) is not the same as column 1's length (%d)", i+1, length(VECTOR_ELT(list_of_columns, i)), nrows);
  }
  const Rboolean verbose = LOGICAL(verboseArg)[0];
  const Rboolean quote = LOGICAL(quoteArg)[0];
  
  const char col_sep = *CHAR(STRING_ELT(col_sep_Arg, 0));  // DO NOT DO: allow multichar separator (bad idea)
  
  const char *row_sep = CHAR(STRING_ELT(row_sep_Arg, 0));
  const int row_sep_len = strlen(row_sep);  // someone somewhere might want a trailer on every line
  const char *na_str = CHAR(STRING_ELT(na_Arg, 0));
  const int na_len = strlen(na_str);
  const char QUOTE = '"';
  const char ESCAPE = '\\';
  const Rboolean qmethod_escape = LOGICAL(qmethod_escapeArg)[0];
  const char ESCAPE_QUOTE = qmethod_escape ? ESCAPE : QUOTE;
  const char *filename = CHAR(STRING_ELT(filenameArg, 0));

  errno = 0;   // clear flag possibly set by previous errors
#ifdef WIN32
  int f = _open(filename, _O_WRONLY | _O_BINARY | _O_CREAT | (LOGICAL(append)[0] ? _O_APPEND : _O_TRUNC), _S_IWRITE);
  // row_sep must be passed from R level as '\r\n' on Windows since write() only auto-converts \n to \r\n
  // in _O_TEXT mode. We use O_BINARY for full control and perhaps speed since O_TEXT must have to deep branch an if('\n')
#else
  int f = open(filename, O_WRONLY | O_CREAT | (LOGICAL(append)[0] ? O_APPEND : O_TRUNC), 0644);
#endif
  if (f == -1) {
    if( access( filename, F_OK ) != -1 )
      error("File exists and failed to open for writing. Do you have write permission to it? Is this Windows and does another process such as Excel have it open? File: %s", filename);
    else 
      error("Unable to create new file for writing (it does not exist already). Do you have permission to write here and is there space on the disk? File: %s", filename); 
  }
  int true_false;
  
  clock_t t0=clock();
  // i) prefetch levels of factor columns (if any) to save getAttrib on every field on every row of any factor column
  // ii) calculate certain upper bound of line length
  SEXP levels[ncols];  // on-stack vla
  int lineLenMax = 2;  // initialize with eol max width of \r\n on windows
  for (int col_i=0; col_i<ncols; col_i++) {
    SEXP column = VECTOR_ELT(list_of_columns, col_i);
    switch(TYPEOF(column)) {
    case LGLSXP:
      lineLenMax+=5;  // width of FALSE
      break;
    case REALSXP:
      lineLenMax+=20;   // 15 (+ 5 for safety)
      break;
    case INTSXP:
      if (isFactor(column)) {
        levels[col_i] = getAttrib(column, R_LevelsSymbol);
        lineLenMax += maxStrLen(levels[col_i], na_len)*(1+quote) + quote*2;
        //                                    ^^^^^^^^^^ in case every character in the field is a quote, each to be escaped (!)
      } else {
        levels[col_i] = NULL;
        lineLenMax+=11;   // 11 + sign
      }
      break;
    case STRSXP:
      lineLenMax += maxStrLen(column, na_len)*(1+quote) + quote*2;
      break;
    default:
      error("Column %d's type is '%s' - not yet implemented.", col_i+1,type2char(TYPEOF(column)) );
    }
    lineLenMax++;  // column separator
  }
  clock_t tlineLenMax=clock()-t0;
  if (verbose) Rprintf("Maximum line length is %d calculated in %.3fs\n", lineLenMax, 1.0*tlineLenMax/CLOCKS_PER_SEC);
  // TODO: could parallelize by column, but currently no need as insignificant time
  t0=clock();

  if (verbose) Rprintf("Writing column names ... ");
  if (LOGICAL(col_names)[0]) {
    SEXP names = getAttrib(list_of_columns, R_NamesSymbol);  
    if (names!=NULL) {
      if (LENGTH(names) != ncols) error("Internal error: length of column names is not equal to the number of columns. Please report.");
      int bufSize = 0;
      for (int col_i=0; col_i<ncols; col_i++) bufSize += LENGTH(STRING_ELT(names, col_i));
      bufSize *= 1+quote;  // in case every colname is filled with quotes to be escaped!
      bufSize += ncols*(2*quote + 1) + 3;
      char *buffer = malloc(bufSize);
      if (buffer == NULL) error("Unable to allocate %dMB buffer for column names", bufSize/(1024*1024));
      char *ch = buffer;
      for (int col_i=0; col_i<ncols; col_i++) {
        SEXP str = STRING_ELT(names, col_i);
        if (str==NA_STRING) {
          if (na_len) { memcpy(ch, na_str, na_len); ch += na_len; }
          break;
        }
        if (quote) {
          QUOTE_FIELD;
        } else {
          memcpy(ch, CHAR(str), LENGTH(str));
          ch += LENGTH(str);
        }
        *ch++ = col_sep;
      }
      ch--;  // backup onto the last col_sep after the last column
      memcpy(ch, row_sep, row_sep_len);  // replace it with the newline 
      ch += row_sep_len;
      if (WRITE(f, buffer, (int)(ch-buffer)) == -1) { close(f); error("Error writing to file: %s", filename); }
      free(buffer);
    }
  }
  if (verbose) Rprintf("done in %.3fs\n", 1.0*(clock()-t0)/CLOCKS_PER_SEC);
  if (nrows == 0) {
    if (verbose) Rprintf("No data rows present (nrow==0)\n");
    if (CLOSE(f)) error("Error closing file: %s", filename);
    return(R_NilValue);
  }

  // Decide buffer size on each core
  // Large enough to fit many lines (to reduce calls to write()). Small enough to fit in each core's cache.
  // If the lines turn out smaller, that's ok. we just won't use all the buffer in that case. But we must be
  // sure to allow for worst case; i.e. every row in the batch all being the maximum line length.
  int bufSize = 1*1024*1024;  // 1MB  TODO: experiment / fetch cache size
  if (lineLenMax > bufSize) bufSize = lineLenMax;
  const int rowsPerBatch = bufSize/lineLenMax;
  const int numBatches = (nrows-1)/rowsPerBatch + 1;
  if (verbose) Rprintf("Writing data rows in %d batches of %d rows (each buffer size %.3fMB) ... ",
    numBatches, rowsPerBatch, 1.0*bufSize/(1024*1024));
  t0 = clock();
  
  int nth;
  #pragma omp parallel
  {
    char *buffer = malloc(bufSize);  // one buffer per thread
    // TODO Ask Norm how to error() safely ... if (buffer == NULL) error("Unable to allocate %dMB buffer", bufSize/(1024*1024)); 
    char *ch = buffer;
    #pragma omp single
    {
      nth = omp_get_num_threads();
    }    
    #pragma omp for ordered schedule(dynamic)
    for(RLEN start_row = 0; start_row < nrows; start_row += rowsPerBatch) { 
      int upp = start_row + rowsPerBatch;
      if (upp > nrows) upp = nrows;
      for (RLEN row_i = start_row; row_i < upp; row_i++) {
        for (int col_i = 0; col_i < ncols; col_i++) {
          SEXP column = VECTOR_ELT(list_of_columns, col_i);
          SEXP str;
          switch(TYPEOF(column)) {
          case LGLSXP:
            true_false = LOGICAL(column)[row_i];
            if (true_false == NA_LOGICAL) {
              if (na_len) { memcpy(ch, na_str, na_len); ch += na_len; }
            } else if (true_false) {
              memcpy(ch,"TRUE",4);   // Other than strings, field widths are limited which we check elsewhere here to ensure
              ch += 4;
            } else {
              memcpy(ch,"FALSE",5);
              ch += 5;
            }
            break;
          case REALSXP:
            if (ISNA(REAL(column)[row_i])) {
              if (na_len) { memcpy(ch, na_str, na_len); ch += na_len; }
            } else {
              //tt0 = clock();
              ch += sprintf(ch, "%.15G", REAL(column)[row_i]);
              //tNUM += clock()-tt0;
            }
            break;
          case INTSXP:
            if (INTEGER(column)[row_i] == NA_INTEGER) {
              if (na_len) { memcpy(ch, na_str, na_len); ch += na_len; }
            } else if (levels[col_i] != NULL) {   // isFactor(column) == TRUE
              str = STRING_ELT(levels[col_i], INTEGER(column)[row_i]-1);
              if (quote) {
                QUOTE_FIELD;
              } else {
                memcpy(ch, CHAR(str), LENGTH(str));
                ch += LENGTH(str);
              }
            } else {
              ch += sprintf(ch, "%d", INTEGER(column)[row_i]);
            }
            break;
          case STRSXP:
            str = STRING_ELT(column, row_i);
            if (str==NA_STRING) {
              if (na_len) { memcpy(ch, na_str, na_len); ch += na_len; }
            } else if (quote) {
              QUOTE_FIELD;
            } else {
              //tt0 = clock();
              memcpy(ch, CHAR(str), LENGTH(str));  // could have large fields. Doubt call overhead is much of an issue on small fields.
              ch += LENGTH(str);
              //tSTR += clock()-tt0;
            }
            break;
          // default:
          // An uncovered type would have already thrown above when calculating maxLineLen earlier
          }
          *ch++ = col_sep;
        }
        ch--;  // backup onto the last col_sep after the last column
        memcpy(ch, row_sep, row_sep_len);  // replace it with the newline. TODO: replace memcpy call with eol1 eol2 --eolLen 
        ch += row_sep_len;
      }
      #pragma omp ordered
      {
        WRITE(f, buffer, (int)(ch-buffer));
        // TODO: safe way to throw  if ( == -1) { close(f); error("Error writing to file: %s", filename);
        ch = buffer;
      }
    }
    free(buffer);
  }
  if (verbose) Rprintf("all %d threads done\n", nth);  // TO DO: report elapsed time since (clock()-t0)/NTH is only estimate
  if (CLOSE(f)) error("Error closing file: %s", filename);
  return(R_NilValue);  // must always return SEXP from C level otherwise hang on Windows
}



