/*
 * tclConfig.c --
 *
 *	This file provides the facilities which allow Tcl and other packages
 *	to embed configuration information into their binary libraries.
 *
 * Copyright (c) 2002 Andreas Kupries <andreas_kupries@users.sourceforge.net>
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"

/*
 * Internal structure to hold embedded configuration information.
 *
 * Our structure is a two-level dictionary associated with the 'interp'. The
 * first level is keyed with the package name and maps to the dictionary for
 * that package. The package dictionary is keyed with metadata keys and maps
 * to the metadata value for that key. This is package specific. The metadata
 * values are in UTF-8, converted from the external representation given to us
 * by the caller.
 */

#define ASSOC_KEY	"tclPackageAboutDict"

/*
 * A ClientData struct for the QueryConfig command.  Store the two bits
 * of data we need; the package name for which we store a config dict,
 * and the (Tcl_Interp *) in which it is stored.
 */

typedef struct QCCD {
    Tcl_Obj *pkg;
    Tcl_Interp *interp;
} QCCD;

/*
 * Static functions in this file:
 */

static Tcl_ObjCmdProc	QueryConfigObjCmd;
static void		QueryConfigDelete(ClientData clientData);
static Tcl_Obj *	GetConfigDict(Tcl_Interp *interp);
static void		ConfigDictDeleteProc(ClientData clientData,
			    Tcl_Interp *interp);

/*
 *----------------------------------------------------------------------
 *
 * Tcl_RegisterConfig --
 *
 *	See TIP#59 for details on what this function does.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Creates namespace and cfg query command in it as per TIP #59.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_RegisterConfig(
    Tcl_Interp *interp,		/* Interpreter the configuration command is
				 * registered in. */
    const char *pkgName,	/* Name of the package registering the
				 * embedded configuration. ASCII, thus in
				 * UTF-8 too. */
    const Tcl_Config *configuration,	/* Embedded configuration. */
    const char *valEncoding)	/* Name of the encoding used to store the
				 * configuration values, ASCII, thus UTF-8. */
{
    Tcl_DString cmdName;
    const Tcl_Config *cfg;
    Tcl_Encoding venc = Tcl_GetEncoding(NULL, valEncoding);
    QCCD *cdPtr = ckalloc(sizeof(QCCD));

    cdPtr->interp = interp;
    cdPtr->pkg = Tcl_NewStringObj(pkgName, TCL_STRLEN);

    /*
     * Phase I: Adding the provided information to the internal database of
     * package meta data. Only if we have an ok encoding.
     *
     * Phase II: Create a command for querying this database, specific to the
     * package registerting its configuration. This is the approved interface
     * in TIP 59. In the future a more general interface should be done, as
     * followup to TIP 59. Simply because our database is now general across
     * packages, and not a structure tied to one package.
     *
     * Note, the created command will have a reference through its clientdata.
     */

    Tcl_IncrRefCount(cdPtr->pkg);

    /*
     * For venc == NULL aka bogus encoding we skip the step setting up the
     * dictionaries visible at Tcl level. I.e. they are not filled
     */

    if (venc != NULL) {
	Tcl_Obj *pkgDict, *pDB = GetConfigDict(interp);

	/*
	 * Retrieve package specific configuration...
	 */

	if (Tcl_DictObjGet(interp, pDB, cdPtr->pkg, &pkgDict) != TCL_OK
		|| (pkgDict == NULL)) {
	    pkgDict = Tcl_NewDictObj();
	} else if (Tcl_IsShared(pkgDict)) {
	    pkgDict = Tcl_DuplicateObj(pkgDict);
	}

	/*
	 * Extend the package configuration...
	 */

	for (cfg=configuration ; cfg->key!=NULL && cfg->key[0]!='\0' ; cfg++) {
	    Tcl_DString conv;
	    const char *convValue =
		    Tcl_ExternalToUtfDString(venc, cfg->value, TCL_STRLEN,
			    &conv);

	    /*
	     * We know that the keys are in ASCII/UTF-8, so for them is no
	     * conversion required.
	     */

	    Tcl_DictObjPut(interp, pkgDict,
		    Tcl_NewStringObj(cfg->key, TCL_STRLEN),
		    Tcl_NewStringObj(convValue, TCL_STRLEN));
	    Tcl_DStringFree(&conv);
	}

	/*
	 * We're now done with the encoding, so drop it.
	 */

	Tcl_FreeEncoding(venc);

	/*
	 * Write the changes back into the overall database.
	 */

	Tcl_DictObjPut(interp, pDB, cdPtr->pkg, pkgDict);
    }

    /*
     * Now create the interface command for retrieval of the package
     * information.
     */

    Tcl_DStringInit(&cmdName);
    TclDStringAppendLiteral(&cmdName, "::");
    Tcl_DStringAppend(&cmdName, pkgName, TCL_STRLEN);

    /*
     * The incomplete command name is the name of the namespace to place it
     * in.
     */

    if (Tcl_FindNamespace(interp, Tcl_DStringValue(&cmdName), NULL,
	    TCL_GLOBAL_ONLY) == NULL) {
	if (Tcl_CreateNamespace(interp, Tcl_DStringValue(&cmdName),
		NULL, NULL) == NULL) {
	    Tcl_Panic("%s.\n%s: %s",
		    Tcl_GetStringResult(interp), "Tcl_RegisterConfig",
		    "Unable to create namespace for package configuration.");
	}
    }

    TclDStringAppendLiteral(&cmdName, "::pkgconfig");

    if (Tcl_CreateObjCommand(interp, Tcl_DStringValue(&cmdName),
	    QueryConfigObjCmd, cdPtr, QueryConfigDelete) == NULL) {
	Tcl_Panic("%s: %s", "Tcl_RegisterConfig",
		"Unable to create query command for package configuration");
    }

    Tcl_DStringFree(&cmdName);
}

/*
 *----------------------------------------------------------------------
 *
 * QueryConfigObjCmd --
 *
 *	Implementation of "::<package>::pkgconfig", the command to query
 *	configuration information embedded into a binary library.
 *
 * Results:
 *	A standard tcl result.
 *
 * Side effects:
 *	See the manual for what this command does.
 *
 *----------------------------------------------------------------------
 */

static int
QueryConfigObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    size_t objc,
    struct Tcl_Obj *const *objv)
{
    QCCD *cdPtr = clientData;
    Tcl_Obj *pkgName = cdPtr->pkg;
    Tcl_Obj *pDB, *pkgDict, *val, *listPtr;
    int index;
    size_t n;
    static const char *const subcmdStrings[] = {
	"get", "list", NULL
    };
    enum subcmds {
	CFG_GET, CFG_LIST
    };

    if ((objc < 2) || (objc > 3)) {
	Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?arg?");
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], subcmdStrings, "subcommand", 0,
	    &index) != TCL_OK) {
	return TCL_ERROR;
    }

    pDB = GetConfigDict(interp);
    if (Tcl_DictObjGet(interp, pDB, pkgName, &pkgDict) != TCL_OK
	    || pkgDict == NULL) {
	/*
	 * Maybe a Tcl_Panic is better, because the package data has to be
	 * present.
	 */

	Tcl_SetObjResult(interp, Tcl_NewStringObj("package not known",
		TCL_STRLEN));
	Tcl_SetErrorCode(interp, "TCL", "FATAL", "PKGCFG_BASE",
		Tcl_GetString(pkgName), NULL);
	return TCL_ERROR;
    }

    switch ((enum subcmds) index) {
    case CFG_GET:
	if (objc != 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "key");
	    return TCL_ERROR;
	}

	if (Tcl_DictObjGet(interp, pkgDict, objv[2], &val) != TCL_OK
		|| val == NULL) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj("key not known",
		    TCL_STRLEN));
	    Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "CONFIG",
		    Tcl_GetString(objv[2]), NULL);
	    return TCL_ERROR;
	}

	Tcl_SetObjResult(interp, val);
	return TCL_OK;

    case CFG_LIST:
	if (objc != 2) {
	    Tcl_WrongNumArgs(interp, 2, objv, NULL);
	    return TCL_ERROR;
	}

	Tcl_DictObjSize(interp, pkgDict, &n);
	listPtr = Tcl_NewListObj(n, NULL);

	if (!listPtr) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    "insufficient memory to create list", TCL_STRLEN));
	    Tcl_SetErrorCode(interp, "TCL", "MEMORY", NULL);
	    return TCL_ERROR;
	}

	if (n) {
	    Tcl_DictSearch s;
	    Tcl_Obj *key;
	    int done;

	    for (Tcl_DictObjFirst(interp, pkgDict, &s, &key, NULL, &done);
		    !done; Tcl_DictObjNext(&s, &key, NULL, &done)) {
		Tcl_ListObjAppendElement(NULL, listPtr, key);
	    }
	}

	Tcl_SetObjResult(interp, listPtr);
	return TCL_OK;

    default:
	Tcl_Panic("QueryConfigObjCmd: Unknown subcommand to 'pkgconfig'. This can't happen");
	break;
    }
    return TCL_ERROR;
}

/*
 *-------------------------------------------------------------------------
 *
 * QueryConfigDelete --
 *
 *	Command delete function. Cleans up after the configuration query
 *	command when it is deleted by the user or during finalization.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Deallocates all non-transient memory allocated by Tcl_RegisterConfig.
 *
 *-------------------------------------------------------------------------
 */

static void
QueryConfigDelete(
    ClientData clientData)
{
    QCCD *cdPtr = clientData;
    Tcl_Obj *pkgName = cdPtr->pkg;
    Tcl_Obj *pDB = GetConfigDict(cdPtr->interp);

    Tcl_DictObjRemove(NULL, pDB, pkgName);
    Tcl_DecrRefCount(pkgName);
    ckfree(cdPtr);
}

/*
 *-------------------------------------------------------------------------
 *
 * GetConfigDict --
 *
 *	Retrieve the package metadata database from the interpreter.
 *	Initializes it, if not present yet.
 *
 * Results:
 *	A Tcl_Obj reference
 *
 * Side effects:
 *	May allocate a Tcl_Obj.
 *
 *-------------------------------------------------------------------------
 */

static Tcl_Obj *
GetConfigDict(
    Tcl_Interp *interp)
{
    Tcl_Obj *pDB = Tcl_GetAssocData(interp, ASSOC_KEY, NULL);

    if (pDB == NULL) {
	pDB = Tcl_NewDictObj();
	Tcl_IncrRefCount(pDB);
	Tcl_SetAssocData(interp, ASSOC_KEY, ConfigDictDeleteProc, pDB);
    }

    return pDB;
}

/*
 *----------------------------------------------------------------------
 *
 * ConfigDictDeleteProc --
 *
 *	This function is associated with the "Package About dict" assoc data
 *	for an interpreter; it is invoked when the interpreter is deleted in
 *	order to free the information assoicated with any pending error
 *	reports.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The package metadata database is freed.
 *
 *----------------------------------------------------------------------
 */

static void
ConfigDictDeleteProc(
    ClientData clientData,	/* Pointer to Tcl_Obj. */
    Tcl_Interp *interp)		/* Interpreter being deleted. */
{
    Tcl_Obj *pDB = clientData;

    Tcl_DecrRefCount(pDB);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
