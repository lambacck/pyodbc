
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "pyodbc.h"
#include "pyodbcmodule.h"
#include "connection.h"
#include "cursor.h"
#include "row.h"
#include "wrapper.h"
#include "errors.h"
#include "getdata.h"
#include "cnxninfo.h"
#include "dbspecific.h"
#include "sqlwchar.h"

#include <time.h>
#include <stdarg.h>

_typeobject* OurDateTimeType = 0;
_typeobject* OurDateType = 0;
_typeobject* OurTimeType = 0;

PyObject* pModule = 0;

static char pyodbc_doc[] =
    "A database module for accessing databases via ODBC.\n"
    "\n"
    "This module conforms to the DB API 2.0 specification while providing\n"
    "non-standard convenience features.  Only standard Python data types are used\n"
    "so additional DLLs are not required.\n"
    "\n"
    "Static Variables:\n\n"
    "version\n"
    "  The module version string.  Official builds will have a version in the format\n"
    "  `major.minor.revision`, such as 2.1.7.  Beta versions will have -beta appended,\n"
    "  such as 2.1.8-beta03.  (This would be a build before the official 2.1.8 release.)\n"
    "  Some special test builds will have a test name (the git branch name) prepended,\n"
    "  such as fixissue90-2.1.8-beta03.\n"
    "\n"
    "apilevel\n"
    "  The string constant '2.0' indicating this module supports DB API level 2.0.\n"
    "\n"
    "lowercase\n"
    "  A Boolean that controls whether column names in result rows are lowercased.\n"
    "  This can be changed any time and affects queries executed after the change.\n"
    "  The default is False.  This can be useful when database columns have\n"
    "  inconsistent capitalization.\n"
    "\n"
    "pooling\n"
    "  A Boolean indicating whether connection pooling is enabled.  This is a\n"
    "  global (HENV) setting, so it can only be modified before the first\n"
    "  connection is made.  The default is True, which enables ODBC connection\n"
    "  pooling.\n"
    "\n"
    "threadsafety\n"
    "  The integer 1, indicating that threads may share the module but not\n"
    "  connections.  Note that connections and cursors may be used by different\n"
    "  threads, just not at the same time.\n"
    "\n"
    "qmark\n"
    "  The string constant 'qmark' to indicate parameters are identified using\n"
    "  question marks.";

PyObject* Error;
PyObject* Warning;
PyObject* InterfaceError;
PyObject* DatabaseError;
PyObject* InternalError;
PyObject* OperationalError;
PyObject* ProgrammingError;
PyObject* IntegrityError;
PyObject* DataError;
PyObject* NotSupportedError;

struct ExcInfo
{
    const char* szName;
    const char* szFullName;
    PyObject** ppexc;
    PyObject** ppexcParent;
    const char* szDoc;
};

#define MAKEEXCINFO(name, parent, doc) { #name, "pyodbc." #name, &name, &parent, doc }

static ExcInfo aExcInfos[] = {
    MAKEEXCINFO(Error, PyExc_Exception, 
                "Exception that is the base class of all other error exceptions. You can use\n"
                "this to catch all errors with one single 'except' statement."),
    MAKEEXCINFO(Warning, PyExc_Exception,
                "Exception raised for important warnings like data truncations while inserting,\n"
                " etc."),
    MAKEEXCINFO(InterfaceError, Error,
                "Exception raised for errors that are related to the database interface rather\n"
                "than the database itself."),
    MAKEEXCINFO(DatabaseError, Error, "Exception raised for errors that are related to the database."),
    MAKEEXCINFO(DataError, DatabaseError, 
                "Exception raised for errors that are due to problems with the processed data\n"
                "like division by zero, numeric value out of range, etc."),
    MAKEEXCINFO(OperationalError, DatabaseError,
                "Exception raised for errors that are related to the database's operation and\n"
                "not necessarily under the control of the programmer, e.g. an unexpected\n"
                "disconnect occurs, the data source name is not found, a transaction could not\n"
                "be processed, a memory allocation error occurred during processing, etc."),
    MAKEEXCINFO(IntegrityError, DatabaseError,
                "Exception raised when the relational integrity of the database is affected,\n"
                "e.g. a foreign key check fails."),
    MAKEEXCINFO(InternalError, DatabaseError,
                "Exception raised when the database encounters an internal error, e.g. the\n"
                "cursor is not valid anymore, the transaction is out of sync, etc."),
    MAKEEXCINFO(ProgrammingError, DatabaseError,
                "Exception raised for programming errors, e.g. table not found or already\n"
                "exists, syntax error in the SQL statement, wrong number of parameters\n"
                "specified, etc."),
    MAKEEXCINFO(NotSupportedError, DatabaseError,
                "Exception raised in case a method or database API was used which is not\n"
                "supported by the database, e.g. requesting a .rollback() on a connection that\n"
                "does not support transaction or has transactions turned off.")
};


PyObject* decimal_type;

HENV henv = SQL_NULL_HANDLE;

SQLWCHAR chDecimal        = '.';
SQLWCHAR chGroupSeparator = ',';
SQLWCHAR chCurrencySymbol = '$';

// Initialize the global decimal character and thousands separator character, used when parsing decimal
// objects.
//
static void init_locale_info()
{
    Object module = PyImport_ImportModule("locale");
    if (!module)
    {
        PyErr_Clear();
        return;
    }

    Object ldict = PyObject_CallMethod(module, "localeconv", 0);
    if (!ldict)
    {
        PyErr_Clear();
        return;
    }

    PyObject* value = PyDict_GetItemString(ldict, "decimal_point");
    if (value && PyUnicode_Check(value) && PyUnicode_GET_SIZE(value) == 1)
    {
        chDecimal = (SQLWCHAR)PyUnicode_AsUnicode(value)[0];
    }
        
    value = PyDict_GetItemString(ldict, "thousands_sep");
    if (value && PyUnicode_Check(value) && PyUnicode_GET_SIZE(value) == 1)
    {
        chGroupSeparator = PyUnicode_AsUnicode(value)[0];

        if (chGroupSeparator == '\0')
        {
            // I don't know why, but the default locale isn't setting ','.  We're going to make the assumption that the
            // most common values are ',' and '.', and we'll take the opposite of the decimal value.
            chGroupSeparator = (chDecimal == ',') ? '.' : ',';
        }
    }

    value = PyDict_GetItemString(ldict, "currency_symbol");
    if (value && PyUnicode_Check(value) && PyUnicode_GET_SIZE(value) == 1)
    {
        chCurrencySymbol = PyUnicode_AsUnicode(value)[0];
    }
}


static bool import_types()
{
    // In Python 2.5 final, PyDateTime_IMPORT no longer works unless the datetime module was previously
    // imported (among other problems).

    PyObject* pdt = PyImport_ImportModule("datetime");
    
    if (!pdt)
        return false;

    PyDateTime_IMPORT;
     
    if (!PyDateTimeAPI)
    {
        PyErr_SetString(PyExc_RuntimeError, "Unable to import the datetime module.");
        return false;
    }
    
    OurDateTimeType = PyDateTimeAPI->DateTimeType;
    OurDateType     = PyDateTimeAPI->DateType;
    OurTimeType     = PyDateTimeAPI->TimeType;

    Cursor_init();
    CnxnInfo_init();
    GetData_init();

    PyObject* decimalmod = PyImport_ImportModule("decimal");
    if (!decimalmod)
    {
        PyErr_SetString(PyExc_RuntimeError, "Unable to import decimal");
        return false;
    }
    
    decimal_type = PyObject_GetAttrString(decimalmod, "Decimal");
    Py_DECREF(decimalmod);

    if (decimal_type == 0)
        PyErr_SetString(PyExc_RuntimeError, "Unable to import decimal.Decimal.");
    
    return decimal_type != 0;
}


static bool AllocateEnv()
{
    PyObject* pooling = PyObject_GetAttrString(pModule, "pooling");
    bool bPooling = pooling == Py_True;
    Py_DECREF(pooling);

    if (bPooling)
    {
        if (!SQL_SUCCEEDED(SQLSetEnvAttr(SQL_NULL_HANDLE, SQL_ATTR_CONNECTION_POOLING, (SQLPOINTER)SQL_CP_ONE_PER_HENV, sizeof(int))))
        {
            Py_FatalError("Unable to set SQL_ATTR_CONNECTION_POOLING attribute.");
            return false;
        }
    }
    
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv)))
    {
        Py_FatalError("Can't initialize module pyodbc.  SQLAllocEnv failed.");
        return false;
    }

    if (!SQL_SUCCEEDED(SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, sizeof(int))))
    {
        Py_FatalError("Unable to set SQL_ATTR_ODBC_VERSION attribute.");
        return false;
    }

    return true;
}

// Map DB API recommended keywords to ODBC keywords.

struct keywordmap
{
    const char* oldname;
    const char* newname;
    PyObject* newnameObject;    // PyString object version of newname, created as needed.
};

static keywordmap keywordmaps[] =
{
    { "user",     "uid",    0 },
    { "password", "pwd",    0 },
    { "host",     "server", 0 },
};

static PyObject* autocommitU;
static PyObject* timeoutU;
static PyObject* equalsU;
static PyObject* semiU;

static PyObject* mod_connect(PyObject* self, PyObject* args, PyObject* kwargs)
{
    UNUSED(self);
    
    Py_ssize_t size = args ? PyTuple_Size(args) : 0;

    if (size > 1)
    {
        PyErr_SetString(PyExc_TypeError, "function takes at most 1 non-keyword argument");
        return 0;
    }

    Unicode cstring;
    int fAutoCommit = 0;
    long timeout = 0;

    if (size == 1)
    {
        if (!PyUnicode_Check(PyTuple_GET_ITEM(args, 0)))
            return PyErr_Format(PyExc_TypeError, "argument 1 must be a string");

        cstring.Append(PyTuple_GET_ITEM(args, 0));
    }

    if (kwargs && PyDict_Size(kwargs) > 0)
    {
        if (autocommitU == 0)
        {
            autocommitU = PyUnicode_FromString("autocommit");
            if (!autocommitU)
                return 0;
        }

        if (timeoutU == 0)
        {
            timeoutU = PyUnicode_FromString("timeout");
            if (!timeoutU)
                return 0;
        }

        if (equalsU == 0)
        {
            equalsU = PyUnicode_FromString("=");
            if (!equalsU)
                return 0;
        }

        if (semiU == 0)
        {
            semiU = PyUnicode_FromString(";");
            if (!semiU)
                return 0;
        }

        Py_ssize_t pos = 0;
        PyObject* key = 0;
        PyObject* value = 0;

        while (PyDict_Next(kwargs, &pos, &key, &value))
        {
            // Note: key and value are *borrowed*.

            // Check for non-connection string keywords we accept.  (If we get many more of these, create something
            // table driven.  Are we sure there isn't a Python function to parse keywords but leave those it doesn't
            // know?)
            if (PyUnicode_Compare(key, autocommitU) == 0)
            {
                fAutoCommit = PyObject_IsTrue(value);
                continue;
            }

            if (PyUnicode_Compare(key, timeoutU) == 0)
            {
                timeout = PyLong_AsLong(value);
                if (PyErr_Occurred())
                    return 0;
                continue;
            }
        
            // Anything else must be a string that is appended, along with the keyword to the connection string.

            if (!PyUnicode_Check(value))
            {
                return RaiseError(PyExc_TypeError, "The value for keyword '%S' is not a string'", key);
            }
            
            // Map DB API recommended names to ODBC names (e.g. user --> uid).
            for (size_t i = 0; i < _countof(keywordmaps); i++)
            {
                if (PyUnicode_CompareWithASCIIString(key, keywordmaps[i].oldname) == 0)
                {
                    if (keywordmaps[i].newnameObject == 0)
                    {
                        keywordmaps[i].newnameObject = PyUnicode_FromString(keywordmaps[i].newname);
                        if (keywordmaps[i].newnameObject == 0)
                            return 0;
                    }

                    key = keywordmaps[i].newnameObject;
                    break;
                }
            }

            if (cstring.Size() && !cstring.Append(semiU))
                return 0;

            if (!cstring.Append(key) || !cstring.Append(equalsU) || !cstring.Append(value))
                return 0;
        }
    }
    
    if (cstring.Size() == 0)
        return PyErr_Format(PyExc_TypeError, "no connection information was passed");

    if (henv == SQL_NULL_HANDLE)
    {
        if (!AllocateEnv())
            return 0;
    }
     
    return (PyObject*)Connection_New(cstring, fAutoCommit != 0, timeout);
}


static PyObject* mod_datasources(PyObject* self)
{
    UNUSED(self);
  
    if (henv == SQL_NULL_HANDLE && !AllocateEnv())
        return 0;

    Object result(PyDict_New());
    if (!result)
        return 0;

    SQLWCHAR szDSN[SQL_MAX_DSN_LENGTH];
    SWORD cbDSN;
    SQLWCHAR szDesc[200];
    SWORD cbDesc;

    SQLUSMALLINT nDirection = SQL_FETCH_FIRST;

    SQLRETURN ret;

    for (;;)
    {
        Py_BEGIN_ALLOW_THREADS
        ret = SQLDataSourcesW(henv, SQL_FETCH_NEXT, szDSN,  _countof(szDSN),  &cbDSN, szDesc, _countof(szDesc), &cbDesc);
        Py_END_ALLOW_THREADS
        if (!SQL_SUCCEEDED(ret))
            break;
        
        if (PyDict_SetItem(result, PyUnicode_FromSQLWCHAR(szDSN), PyUnicode_FromSQLWCHAR(szDesc)) == -1)
            return 0;
        nDirection = SQL_FETCH_NEXT;
    }
    
    if (ret != SQL_NO_DATA)
        return RaiseErrorFromHandle("SQLDataSources", SQL_NULL_HANDLE, SQL_NULL_HANDLE);
    
    return result.Detach();
}



static PyObject*
mod_timefromticks(PyObject* self, PyObject* args)
{
    UNUSED(self);
    
    time_t t = 0;
    struct tm* fields;

    // Sigh...  If a float is passed but we ask for a long, we get a deprecation warning printed to the screen instead
    // of a failure.  Not only is this not documented, it means we can't reliably use PyArg_ParseTuple('l') anywhere!
    
    // if (PyArg_ParseTuple(args, "l", &ticks))

    PyObject* num;
    if (!PyArg_ParseTuple(args, "O", &num))
        return 0;
    
    if (PyLong_Check(num))
        t = PyLong_AsLong(num);
    else if (PyFloat_Check(num))
        t = (long)PyFloat_AS_DOUBLE(num);
    else
    {
        PyErr_SetString(PyExc_TypeError, "TimeFromTicks requires a number.");
        return 0;
    }
    
    fields = localtime(&t);

    return PyTime_FromTime(fields->tm_hour, fields->tm_min, fields->tm_sec, 0);
}

static PyObject*
mod_datefromticks(PyObject* self, PyObject* args)
{
    UNUSED(self);
    return PyDate_FromTimestamp(args);
}

static PyObject*
mod_timestampfromticks(PyObject* self, PyObject* args)
{
    UNUSED(self);
    return PyDateTime_FromTimestamp(args);
}

static char connect_doc[] =
    "connect(str, autocommit=False, ansi=False, timeout=0, **kwargs) --> Connection\n"
    "\n"
    "Accepts an ODBC connection string and returns a new Connection object.\n"
    "\n"
    "The connection string will be passed to SQLDriverConnect, so a DSN connection\n"
    "can be created using:\n"
    "\n"
    "  cnxn = pyodbc.connect('DSN=DataSourceName;UID=user;PWD=password')\n"
    "\n"
    "To connect without requiring a DSN, specify the driver and connection\n"
    "information:\n"
    "\n"
    "  DRIVER={SQL Server};SERVER=localhost;DATABASE=testdb;UID=user;PWD=password\n"
    "\n"
    "Note the use of braces when a value contains spaces.  Refer to SQLDriverConnect\n"
    "documentation or the documentation of your ODBC driver for details.\n"
    "\n"
    "The connection string can be passed as the string `str`, as a list of keywords,\n"
    "or a combination of the two.  Any keywords except autocommit, ansi, and timeout\n"
    "(see below) are simply added to the connection string.\n"
    "\n"
    "  connect('server=localhost;user=me')\n"
    "  connect(server='localhost', user='me')\n"
    "  connect('server=localhost', user='me')\n"
    "\n"
    "The DB API recommends the keywords 'user', 'password', and 'host', but these\n"
    "are not valid ODBC keywords, so these will be converted to 'uid', 'pwd', and\n"
    "'server'.\n"
    "\n"
    "Special Keywords\n"
    "\n"
    "The following specal keywords are processed by pyodbc and are not added to the\n"
    "connection string.  (If you must use these in your connection string, pass them\n"
    "as a string, not as keywords.)\n"
    "\n"
    "  autocommit\n"
    "    If False or zero, the default, transactions are created automatically as\n"
    "    defined in the DB API 2.  If True or non-zero, the connection is put into\n"
    "    ODBC autocommit mode and statements are committed automatically.\n"
    "   \n"
    "  ansi\n"
    "    By default, pyodbc first attempts to connect using the Unicode version of\n"
    "    SQLDriverConnectW.  If the driver returns IM001 indicating it does not\n"
    "    support the Unicode version, the ANSI version is tried.  Any other SQLSTATE\n"
    "    is turned into an exception.  Setting ansi to true skips the Unicode\n"
    "    attempt and only connects using the ANSI version.  This is useful for\n"
    "    drivers that return the wrong SQLSTATE (or if pyodbc is out of date and\n"
    "    should support other SQLSTATEs).\n"
    "   \n"
    "  timeout\n"
    "    An integer login timeout in seconds, used to set the SQL_ATTR_LOGIN_TIMEOUT\n"
    "    attribute of the connection.  The default is 0 which means the database's\n"
    "    default timeout, if any, is used.\n";

static char timefromticks_doc[] = 
    "TimeFromTicks(ticks) --> datetime.time\n"
    "\n"
    "Returns a time object initialized from the given ticks value (number of seconds\n"
    "since the epoch; see the documentation of the standard Python time module for\n"
    "details).";

static char datefromticks_doc[] = 
    "DateFromTicks(ticks) --> datetime.date\n"  \
    "\n"                                                                \
    "Returns a date object initialized from the given ticks value (number of seconds\n" \
    "since the epoch; see the documentation of the standard Python time module for\n" \
    "details).";

static char timestampfromticks_doc[] = 
    "TimestampFromTicks(ticks) --> datetime.datetime\n"  \
    "\n"                                                                \
    "Returns a datetime object initialized from the given ticks value (number of\n" \
    "seconds since the epoch; see the documentation of the standard Python time\n" \
    "module for details";

static char datasources_doc[] =
    "dataSources() -> { DSN : Description }\n" \
    "\n" \
    "Returns a dictionary mapping available DSNs to their descriptions.";


#ifdef PYODBC_LEAK_CHECK
static PyObject* mod_leakcheck(PyObject* self, PyObject* args)
{
    UNUSED(self, args);
    pyodbc_leak_check();
    Py_RETURN_NONE;
}
#endif

#ifdef WINVER
static char drivers_doc[] = "drivers() -> [ driver, ... ]\n\nReturns a list of installed drivers";

static PyObject* mod_drivers(PyObject* self, PyObject* args)
{
    UNUSED(self, args);

    RegKey key;
    long ret = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\ODBC\\ODBCINST.INI\\ODBC Drivers", 0, KEY_QUERY_VALUE, &key.hkey);
    if (ret != ERROR_SUCCESS)
        return PyErr_Format(PyExc_RuntimeError, "Unable to access the driver list in the registry.  error=%ld", ret);
 
    Object results(PyList_New(0));
    DWORD index = 0;
    wchar_t name[255];
    DWORD length = _countof(name);

    while (RegEnumValueW(key, index++, name, &length, 0, 0, 0, 0) == ERROR_SUCCESS)
    {
        if (ret != ERROR_SUCCESS)
            return PyErr_Format(PyExc_RuntimeError, "RegEnumKeyEx failed with error %ld\n", ret);

        PyObject* oname = PyUnicode_FromWideChar(name, length);
        if (!oname)
            return 0;
        
        if (PyList_Append(results.Get(), oname) != 0)
        {
            Py_DECREF(oname);
            return 0;
        }
        length = _countof(name);
    }

    return results.Detach();
}
#endif

static PyMethodDef pyodbc_methods[] =
{
    { "connect",            (PyCFunction)mod_connect,            METH_VARARGS|METH_KEYWORDS, connect_doc },
    { "TimeFromTicks",      (PyCFunction)mod_timefromticks,      METH_VARARGS,               timefromticks_doc },
    { "DateFromTicks",      (PyCFunction)mod_datefromticks,      METH_VARARGS,               datefromticks_doc },
    { "TimestampFromTicks", (PyCFunction)mod_timestampfromticks, METH_VARARGS,               timestampfromticks_doc },
    { "dataSources",        (PyCFunction)mod_datasources,        METH_NOARGS,                datasources_doc },

#ifdef WINVER
    { "drivers", (PyCFunction)mod_drivers, METH_NOARGS, drivers_doc },
#endif

#ifdef PYODBC_LEAK_CHECK
    { "leakcheck", (PyCFunction)mod_leakcheck, METH_NOARGS, 0 },
#endif

    { 0, 0, 0, 0 }
};


static void ErrorInit()
{
    // Called during startup to initialize any variables that will be freed by ErrorCleanup.

    Error = 0;
    Warning = 0;
    InterfaceError = 0;
    DatabaseError = 0;
    InternalError = 0;
    OperationalError = 0;
    ProgrammingError = 0;
    IntegrityError = 0;
    DataError = 0;
    NotSupportedError = 0;
    decimal_type = 0;
}


static void ErrorCleanup()
{
    // Called when an error occurs during initialization to release any objects we may have accessed.  Make sure each
    // item released was initialized to zero.  (Static objects are -- non-statics should be initialized in ErrorInit.)

    Py_XDECREF(Error);
    Py_XDECREF(Warning);
    Py_XDECREF(InterfaceError);
    Py_XDECREF(DatabaseError);
    Py_XDECREF(InternalError);
    Py_XDECREF(OperationalError);
    Py_XDECREF(ProgrammingError);
    Py_XDECREF(IntegrityError);
    Py_XDECREF(DataError);
    Py_XDECREF(NotSupportedError);
    Py_XDECREF(decimal_type);
}

struct ConstantDef
{
    const char* szName;
    int value;
};

#define MAKECONST(v) { #v, v }

static const ConstantDef aConstants[] = {
    MAKECONST(SQL_UNKNOWN_TYPE),
    MAKECONST(SQL_CHAR),
    MAKECONST(SQL_VARCHAR),
    MAKECONST(SQL_LONGVARCHAR),
    MAKECONST(SQL_WCHAR),
    MAKECONST(SQL_WVARCHAR),
    MAKECONST(SQL_WLONGVARCHAR),
    MAKECONST(SQL_DECIMAL),
    MAKECONST(SQL_NUMERIC),
    MAKECONST(SQL_SMALLINT),
    MAKECONST(SQL_INTEGER),
    MAKECONST(SQL_REAL),
    MAKECONST(SQL_FLOAT),
    MAKECONST(SQL_DOUBLE),
    MAKECONST(SQL_BIT),
    MAKECONST(SQL_TINYINT),
    MAKECONST(SQL_BIGINT),
    MAKECONST(SQL_BINARY),
    MAKECONST(SQL_VARBINARY),
    MAKECONST(SQL_LONGVARBINARY),
    MAKECONST(SQL_TYPE_DATE),
    MAKECONST(SQL_TYPE_TIME),
    MAKECONST(SQL_TYPE_TIMESTAMP),
    MAKECONST(SQL_SS_TIME2),
    MAKECONST(SQL_SS_XML),
    MAKECONST(SQL_INTERVAL_MONTH),
    MAKECONST(SQL_INTERVAL_YEAR),
    MAKECONST(SQL_INTERVAL_YEAR_TO_MONTH),
    MAKECONST(SQL_INTERVAL_DAY),
    MAKECONST(SQL_INTERVAL_HOUR),
    MAKECONST(SQL_INTERVAL_MINUTE),
    MAKECONST(SQL_INTERVAL_SECOND),
    MAKECONST(SQL_INTERVAL_DAY_TO_HOUR),
    MAKECONST(SQL_INTERVAL_DAY_TO_MINUTE),
    MAKECONST(SQL_INTERVAL_DAY_TO_SECOND),
    MAKECONST(SQL_INTERVAL_HOUR_TO_MINUTE),
    MAKECONST(SQL_INTERVAL_HOUR_TO_SECOND),
    MAKECONST(SQL_INTERVAL_MINUTE_TO_SECOND),
    MAKECONST(SQL_GUID),
    MAKECONST(SQL_NULLABLE),
    MAKECONST(SQL_NO_NULLS),
    MAKECONST(SQL_NULLABLE_UNKNOWN),
    // MAKECONST(SQL_INDEX_BTREE),
    // MAKECONST(SQL_INDEX_CLUSTERED),
    // MAKECONST(SQL_INDEX_CONTENT),
    // MAKECONST(SQL_INDEX_HASHED),
    // MAKECONST(SQL_INDEX_OTHER),
    MAKECONST(SQL_SCOPE_CURROW),
    MAKECONST(SQL_SCOPE_TRANSACTION),
    MAKECONST(SQL_SCOPE_SESSION),
    MAKECONST(SQL_PC_UNKNOWN),
    MAKECONST(SQL_PC_NOT_PSEUDO),
    MAKECONST(SQL_PC_PSEUDO),

    // SQLGetInfo
    MAKECONST(SQL_ACCESSIBLE_PROCEDURES),
    MAKECONST(SQL_ACCESSIBLE_TABLES),
    MAKECONST(SQL_ACTIVE_ENVIRONMENTS),
    MAKECONST(SQL_AGGREGATE_FUNCTIONS),
    MAKECONST(SQL_ALTER_DOMAIN),
    MAKECONST(SQL_ALTER_TABLE),
    MAKECONST(SQL_ASYNC_MODE),
    MAKECONST(SQL_BATCH_ROW_COUNT),
    MAKECONST(SQL_BATCH_SUPPORT),
    MAKECONST(SQL_BOOKMARK_PERSISTENCE),
    MAKECONST(SQL_CATALOG_LOCATION),
    MAKECONST(SQL_CATALOG_NAME),
    MAKECONST(SQL_CATALOG_NAME_SEPARATOR),
    MAKECONST(SQL_CATALOG_TERM),
    MAKECONST(SQL_CATALOG_USAGE),
    MAKECONST(SQL_COLLATION_SEQ),
    MAKECONST(SQL_COLUMN_ALIAS),
    MAKECONST(SQL_CONCAT_NULL_BEHAVIOR),
    MAKECONST(SQL_CONVERT_FUNCTIONS),
    MAKECONST(SQL_CONVERT_VARCHAR),
    MAKECONST(SQL_CORRELATION_NAME),
    MAKECONST(SQL_CREATE_ASSERTION),
    MAKECONST(SQL_CREATE_CHARACTER_SET),
    MAKECONST(SQL_CREATE_COLLATION),
    MAKECONST(SQL_CREATE_DOMAIN),
    MAKECONST(SQL_CREATE_SCHEMA),
    MAKECONST(SQL_CREATE_TABLE),
    MAKECONST(SQL_CREATE_TRANSLATION),
    MAKECONST(SQL_CREATE_VIEW),
    MAKECONST(SQL_CURSOR_COMMIT_BEHAVIOR),
    MAKECONST(SQL_CURSOR_ROLLBACK_BEHAVIOR),
    // MAKECONST(SQL_CURSOR_ROLLBACK_SQL_CURSOR_SENSITIVITY),
    MAKECONST(SQL_DATABASE_NAME),
    MAKECONST(SQL_DATA_SOURCE_NAME),
    MAKECONST(SQL_DATA_SOURCE_READ_ONLY),
    MAKECONST(SQL_DATETIME_LITERALS),
    MAKECONST(SQL_DBMS_NAME),
    MAKECONST(SQL_DBMS_VER),
    MAKECONST(SQL_DDL_INDEX),
    MAKECONST(SQL_DEFAULT_TXN_ISOLATION),
    MAKECONST(SQL_DESCRIBE_PARAMETER),
    MAKECONST(SQL_DM_VER),
    MAKECONST(SQL_DRIVER_HDESC),
    MAKECONST(SQL_DRIVER_HENV),
    MAKECONST(SQL_DRIVER_HLIB),
    MAKECONST(SQL_DRIVER_HSTMT),
    MAKECONST(SQL_DRIVER_NAME),
    MAKECONST(SQL_DRIVER_ODBC_VER),
    MAKECONST(SQL_DRIVER_VER),
    MAKECONST(SQL_DROP_ASSERTION),
    MAKECONST(SQL_DROP_CHARACTER_SET),
    MAKECONST(SQL_DROP_COLLATION),
    MAKECONST(SQL_DROP_DOMAIN),
    MAKECONST(SQL_DROP_SCHEMA),
    MAKECONST(SQL_DROP_TABLE),
    MAKECONST(SQL_DROP_TRANSLATION),
    MAKECONST(SQL_DROP_VIEW),
    MAKECONST(SQL_DYNAMIC_CURSOR_ATTRIBUTES1),
    MAKECONST(SQL_DYNAMIC_CURSOR_ATTRIBUTES2),
    MAKECONST(SQL_EXPRESSIONS_IN_ORDERBY),
    MAKECONST(SQL_FILE_USAGE),
    MAKECONST(SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1),
    MAKECONST(SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2),
    MAKECONST(SQL_GETDATA_EXTENSIONS),
    MAKECONST(SQL_GROUP_BY),
    MAKECONST(SQL_IDENTIFIER_CASE),
    MAKECONST(SQL_IDENTIFIER_QUOTE_CHAR),
    MAKECONST(SQL_INDEX_KEYWORDS),
    MAKECONST(SQL_INFO_SCHEMA_VIEWS),
    MAKECONST(SQL_INSERT_STATEMENT),
    MAKECONST(SQL_INTEGRITY),
    MAKECONST(SQL_KEYSET_CURSOR_ATTRIBUTES1),
    MAKECONST(SQL_KEYSET_CURSOR_ATTRIBUTES2),
    MAKECONST(SQL_KEYWORDS),
    MAKECONST(SQL_LIKE_ESCAPE_CLAUSE),
    MAKECONST(SQL_MAX_ASYNC_CONCURRENT_STATEMENTS),
    MAKECONST(SQL_MAX_BINARY_LITERAL_LEN),
    MAKECONST(SQL_MAX_CATALOG_NAME_LEN),
    MAKECONST(SQL_MAX_CHAR_LITERAL_LEN),
    MAKECONST(SQL_MAX_COLUMNS_IN_GROUP_BY),
    MAKECONST(SQL_MAX_COLUMNS_IN_INDEX),
    MAKECONST(SQL_MAX_COLUMNS_IN_ORDER_BY),
    MAKECONST(SQL_MAX_COLUMNS_IN_SELECT),
    MAKECONST(SQL_MAX_COLUMNS_IN_TABLE),
    MAKECONST(SQL_MAX_COLUMN_NAME_LEN),
    MAKECONST(SQL_MAX_CONCURRENT_ACTIVITIES),
    MAKECONST(SQL_MAX_CURSOR_NAME_LEN),
    MAKECONST(SQL_MAX_DRIVER_CONNECTIONS),
    MAKECONST(SQL_MAX_IDENTIFIER_LEN),
    MAKECONST(SQL_MAX_INDEX_SIZE),
    MAKECONST(SQL_MAX_PROCEDURE_NAME_LEN),
    MAKECONST(SQL_MAX_ROW_SIZE),
    MAKECONST(SQL_MAX_ROW_SIZE_INCLUDES_LONG),
    MAKECONST(SQL_MAX_SCHEMA_NAME_LEN),
    MAKECONST(SQL_MAX_STATEMENT_LEN),
    MAKECONST(SQL_MAX_TABLES_IN_SELECT),
    MAKECONST(SQL_MAX_TABLE_NAME_LEN),
    MAKECONST(SQL_MAX_USER_NAME_LEN),
    MAKECONST(SQL_MULTIPLE_ACTIVE_TXN),
    MAKECONST(SQL_MULT_RESULT_SETS),
    MAKECONST(SQL_NEED_LONG_DATA_LEN),
    MAKECONST(SQL_NON_NULLABLE_COLUMNS),
    MAKECONST(SQL_NULL_COLLATION),
    MAKECONST(SQL_NUMERIC_FUNCTIONS),
    MAKECONST(SQL_ODBC_INTERFACE_CONFORMANCE),
    MAKECONST(SQL_ODBC_VER),
    MAKECONST(SQL_OJ_CAPABILITIES),
    MAKECONST(SQL_ORDER_BY_COLUMNS_IN_SELECT),
    MAKECONST(SQL_PARAM_ARRAY_ROW_COUNTS),
    MAKECONST(SQL_PARAM_ARRAY_SELECTS),
    MAKECONST(SQL_PARAM_TYPE_UNKNOWN),
    MAKECONST(SQL_PARAM_INPUT),
    MAKECONST(SQL_PARAM_INPUT_OUTPUT),
    MAKECONST(SQL_PARAM_OUTPUT),
    MAKECONST(SQL_RETURN_VALUE),
    MAKECONST(SQL_RESULT_COL),
    MAKECONST(SQL_PROCEDURES),
    MAKECONST(SQL_PROCEDURE_TERM),
    MAKECONST(SQL_QUOTED_IDENTIFIER_CASE),
    MAKECONST(SQL_ROW_UPDATES),
    MAKECONST(SQL_SCHEMA_TERM),
    MAKECONST(SQL_SCHEMA_USAGE),
    MAKECONST(SQL_SCROLL_OPTIONS),
    MAKECONST(SQL_SEARCH_PATTERN_ESCAPE),
    MAKECONST(SQL_SERVER_NAME),
    MAKECONST(SQL_SPECIAL_CHARACTERS),
    MAKECONST(SQL_SQL92_DATETIME_FUNCTIONS),
    MAKECONST(SQL_SQL92_FOREIGN_KEY_DELETE_RULE),
    MAKECONST(SQL_SQL92_FOREIGN_KEY_UPDATE_RULE),
    MAKECONST(SQL_SQL92_GRANT),
    MAKECONST(SQL_SQL92_NUMERIC_VALUE_FUNCTIONS),
    MAKECONST(SQL_SQL92_PREDICATES),
    MAKECONST(SQL_SQL92_RELATIONAL_JOIN_OPERATORS),
    MAKECONST(SQL_SQL92_REVOKE),
    MAKECONST(SQL_SQL92_ROW_VALUE_CONSTRUCTOR),
    MAKECONST(SQL_SQL92_STRING_FUNCTIONS),
    MAKECONST(SQL_SQL92_VALUE_EXPRESSIONS),
    MAKECONST(SQL_SQL_CONFORMANCE),
    MAKECONST(SQL_STANDARD_CLI_CONFORMANCE),
    MAKECONST(SQL_STATIC_CURSOR_ATTRIBUTES1),
    MAKECONST(SQL_STATIC_CURSOR_ATTRIBUTES2),
    MAKECONST(SQL_STRING_FUNCTIONS),
    MAKECONST(SQL_SUBQUERIES),
    MAKECONST(SQL_SYSTEM_FUNCTIONS),
    MAKECONST(SQL_TABLE_TERM),
    MAKECONST(SQL_TIMEDATE_ADD_INTERVALS),
    MAKECONST(SQL_TIMEDATE_DIFF_INTERVALS),
    MAKECONST(SQL_TIMEDATE_FUNCTIONS),
    MAKECONST(SQL_TXN_CAPABLE),
    MAKECONST(SQL_TXN_ISOLATION_OPTION),
    MAKECONST(SQL_UNION),
    MAKECONST(SQL_USER_NAME),
    MAKECONST(SQL_XOPEN_CLI_YEAR),
};


static bool CreateExceptions()
{
    for (unsigned int i = 0; i < _countof(aExcInfos); i++)
    {
        ExcInfo& info = aExcInfos[i];

        PyObject* classdict = PyDict_New();
        if (!classdict)
            return false;

        PyObject* doc = PyUnicode_FromString(info.szDoc);
        if (!doc)
        {
            Py_DECREF(classdict);
            return false;
        }
        
        PyDict_SetItemString(classdict, "__doc__", doc);
        Py_DECREF(doc);

        *info.ppexc = PyErr_NewException((char*)info.szFullName, *info.ppexcParent, classdict);
        if (*info.ppexc == 0)
        {
            Py_DECREF(classdict);
            return false;
        }
        
        // Keep a reference for our internal (C++) use.
        Py_INCREF(*info.ppexc);

        PyModule_AddObject(pModule, (char*)info.szName, *info.ppexc);
    }

    return true;
}

static struct PyModuleDef pyodbc_module =
{
   PyModuleDef_HEAD_INIT,
   "pyodbc",
   pyodbc_doc,
   -1,
   pyodbc_methods
};

PyMODINIT_FUNC
PyInit_pyodbc()
{
#ifdef _DEBUG
    #ifndef Py_REF_DEBUG
    #error Py_REF_DEBUG not set!
    #endif

    int grfDebugFlags = _CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF;
    _CrtSetDbgFlag(grfDebugFlags);
#endif

    ErrorInit();

    // Make sure that this was built correctly.  Unfortunately, the lack of good static assertions leads to compiler
    // warnings...
    int n1 = sizeof(SQLWCHAR);
    int n2 = SQLWCHAR_SIZE;
    if (n1 != n2)
    {
        PyErr_Format(PyExc_RuntimeError, "The pyodbc module was built with incorrect SQLWCHAR_SIZE: actual-size=%d  compiled-size=%d  version=%s",
                     n1, n2, TOSTRING(PYODBC_VERSION));
        return 0;
    }

    if (PyType_Ready(&ConnectionType) < 0 || PyType_Ready(&CursorType) < 0 || PyType_Ready(&RowType) < 0 || PyType_Ready(&CnxnInfoType) < 0)
        return 0;
    
    pModule = PyModule_Create(&pyodbc_module);

    if (!import_types())
        return 0;

    init_locale_info();

    if (!CreateExceptions())
        return 0;

    const char* szVersion = TOSTRING(PYODBC_VERSION);
    PyModule_AddStringConstant(pModule, "version", szVersion);

    PyModule_AddIntConstant(pModule, "threadsafety", 1);
    PyModule_AddStringConstant(pModule, "apilevel", "2.0");
    PyModule_AddStringConstant(pModule, "paramstyle", "qmark");
    PyModule_AddObject(pModule, "pooling", Py_True);
    Py_INCREF(Py_True);
    PyModule_AddObject(pModule, "lowercase", Py_False);
    Py_INCREF(Py_False);
                       
    PyModule_AddObject(pModule, "Connection", (PyObject*)&ConnectionType);
    Py_INCREF((PyObject*)&ConnectionType);
    PyModule_AddObject(pModule, "Cursor", (PyObject*)&CursorType);
    Py_INCREF((PyObject*)&CursorType);
    PyModule_AddObject(pModule, "Row", (PyObject*)&RowType);
    Py_INCREF((PyObject*)&RowType);

    // Add the SQL_XXX defines from ODBC.
    for (unsigned int i = 0; i < _countof(aConstants); i++)
        PyModule_AddIntConstant(pModule, (char*)aConstants[i].szName, aConstants[i].value);

    PyModule_AddObject(pModule, "Date", (PyObject*)PyDateTimeAPI->DateType);
    Py_INCREF((PyObject*)PyDateTimeAPI->DateType);
    PyModule_AddObject(pModule, "Time", (PyObject*)PyDateTimeAPI->TimeType);
    Py_INCREF((PyObject*)PyDateTimeAPI->TimeType);
    PyModule_AddObject(pModule, "Timestamp", (PyObject*)PyDateTimeAPI->DateTimeType);
    Py_INCREF((PyObject*)PyDateTimeAPI->DateTimeType);
    PyModule_AddObject(pModule, "DATETIME", (PyObject*)PyDateTimeAPI->DateTimeType);
    Py_INCREF((PyObject*)PyDateTimeAPI->DateTimeType);
    PyModule_AddObject(pModule, "STRING", (PyObject*)&PyUnicode_Type);
    Py_INCREF((PyObject*)&PyUnicode_Type);
    PyModule_AddObject(pModule, "NUMBER", (PyObject*)&PyFloat_Type);
    Py_INCREF((PyObject*)&PyFloat_Type);
    PyModule_AddObject(pModule, "ROWID", (PyObject*)&PyLong_Type);
    Py_INCREF((PyObject*)&PyLong_Type);
    PyModule_AddObject(pModule, "BINARY", (PyObject*)&PyBytes_Type);
    Py_INCREF((PyObject*)&PyBytes_Type);
    PyModule_AddObject(pModule, "Binary", (PyObject*)&PyBytes_Type);
    Py_INCREF((PyObject*)&PyBytes_Type);
    
    PyModule_AddIntConstant(pModule, "UNICODE_SIZE", sizeof(Py_UNICODE));
    PyModule_AddIntConstant(pModule, "SQLWCHAR_SIZE", sizeof(SQLWCHAR));

    if (PyErr_Occurred())
    {
        ErrorCleanup();
        return 0;
    }

    return pModule;
}

#ifdef WINVER
BOOL WINAPI DllMain(
  HINSTANCE hMod,
  DWORD fdwReason,
  LPVOID lpvReserved
  )
{
    UNUSED(hMod, fdwReason, lpvReserved);
    return TRUE;
}
#endif
