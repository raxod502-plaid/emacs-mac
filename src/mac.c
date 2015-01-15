/* Unix emulation routines for GNU Emacs on the Mac OS.
   Copyright (C) 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007,
                 2008  Free Software Foundation, Inc.
   Copyright (C) 2009, 2010, 2011  YAMAMOTO Mitsuharu

This file is part of GNU Emacs Mac port.

GNU Emacs Mac port is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

GNU Emacs Mac port is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs Mac port.  If not, see <http://www.gnu.org/licenses/>.  */

/* Originally contributed by Andrew Choi (akochoi@mac.com) for Emacs 21.  */

#include <config.h>

#include <stdio.h>
#include <errno.h>
#include <setjmp.h>

#include "lisp.h"
#include "process.h"
#undef select
#include "systime.h"
#include "sysselect.h"
#include "blockinput.h"

#include "macterm.h"

#include "charset.h"
#include "coding.h"

#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>

#undef init_process
#include <mach/mach.h>
#include <servers/bootstrap.h>
#define init_process emacs_init_process

#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1060
#ifndef SELECT_USE_GCD
#define SELECT_USE_GCD 1
#endif
#endif

#include <sys/socket.h>
#if !SELECT_USE_GCD
#include <pthread.h>
#endif

/* The system script code. */
static EMACS_INT mac_system_script_code;

/* The system locale identifier string.  */
static Lisp_Object Vmac_system_locale;

/* An instance of the AppleScript component.  */
static ComponentInstance as_scripting_component;
/* The single script context used for all script executions.  */
static OSAID as_script_context;


/***********************************************************************
			  Utility functions
 ***********************************************************************/

/* Return the length of the cdr chain of the given LIST.  Return -1 if
   LIST is circular.  */

static EMACS_INT
cdr_chain_length (list)
     Lisp_Object list;
{
  EMACS_INT result = 0;
  Lisp_Object tortoise, hare;

  hare = tortoise = list;

  while (CONSP (hare))
    {
      hare = XCDR (hare);
      result++;
      if (!CONSP (hare))
	break;

      hare = XCDR (hare);
      result++;
      tortoise = XCDR (tortoise);

      if (EQ (hare, tortoise))
	return -1;
    }

  return result;
}

/* Binary search tree to record Lisp objects on the traversal stack,
   used for checking circularity in the conversion from a Lisp object.
   We assume deletion of a node happens only if its children are
   leaves. */

struct bstree_node
{
  Lisp_Object obj;
  struct bstree_node *left, *right;
};

/* Find OBJ in the binary search tree *BSTREE.  If found, the return
   value points to the variable whose value points to the node
   containing OBJ.  Otherwise, the return value points to the variable
   whose value would point to a new node containing OBJ if we added it
   to *BSTREE.  In the latter case, the variable pointed to by the
   return value contains NULL.  */

static struct bstree_node **
bstree_find (bstree, obj)
     struct bstree_node **bstree;
     Lisp_Object obj;
{
  while (*bstree)
    if (XHASH (obj) < XHASH ((*bstree)->obj))
      bstree = &(*bstree)->left;
    else if (XHASH (obj) > XHASH ((*bstree)->obj))
      bstree = &(*bstree)->right;
    else
      break;

  return bstree;
}


/***********************************************************************
		  Conversions on Apple event objects
 ***********************************************************************/

static Lisp_Object Qundecoded_file_name;

static struct {
  AEKeyword keyword;
  const char *name;
  Lisp_Object symbol;
} ae_attr_table [] =
  {{keyTransactionIDAttr,	"transaction-id"},
   {keyReturnIDAttr,		"return-id"},
   {keyEventClassAttr,		"event-class"},
   {keyEventIDAttr,		"event-id"},
   {keyAddressAttr,		"address"},
   {keyOptionalKeywordAttr,	"optional-keyword"},
   {keyTimeoutAttr,		"timeout"},
   {keyInteractLevelAttr,	"interact-level"},
   {keyEventSourceAttr,		"event-source"},
   /* {keyMissedKeywordAttr,	"missed-keyword"}, */
   {keyOriginalAddressAttr,	"original-address"},
   {keyReplyRequestedAttr,	"reply-requested"},
   {KEY_EMACS_SUSPENSION_ID_ATTR, "emacs-suspension-id"}
  };

static Lisp_Object
mac_aelist_to_lisp (desc_list)
     const AEDescList *desc_list;
{
  OSErr err;
  long count;
  Lisp_Object result, elem;
  DescType desc_type;
  Size size;
  AEKeyword keyword;
  AEDesc desc;
  int attribute_p = 0;

  err = AECountItems (desc_list, &count);
  if (err != noErr)
    return Qnil;
  result = Qnil;

 again:
  while (count > 0)
    {
      if (attribute_p)
	{
	  keyword = ae_attr_table[count - 1].keyword;
	  err = AESizeOfAttribute (desc_list, keyword, &desc_type, &size);
	}
      else
	err = AESizeOfNthItem (desc_list, count, &desc_type, &size);

      if (err == noErr)
	switch (desc_type)
	  {
	  case typeAEList:
	  case typeAERecord:
	  case typeAppleEvent:
	    if (attribute_p)
	      err = AEGetAttributeDesc (desc_list, keyword, typeWildCard,
					&desc);
	    else
	      err = AEGetNthDesc (desc_list, count, typeWildCard,
				  &keyword, &desc);
	    if (err != noErr)
	      break;
	    elem = mac_aelist_to_lisp (&desc);
	    AEDisposeDesc (&desc);
	    break;

	  default:
	    if (desc_type == typeNull)
	      elem = Qnil;
	    else
	      {
		elem = make_uninit_string (size);
		if (attribute_p)
		  err = AEGetAttributePtr (desc_list, keyword, typeWildCard,
					   &desc_type, SDATA (elem),
					   size, &size);
		else
		  err = AEGetNthPtr (desc_list, count, typeWildCard, &keyword,
				     &desc_type, SDATA (elem), size, &size);
	      }
	    if (err != noErr)
	      break;
	    desc_type = EndianU32_NtoB (desc_type);
	    elem = Fcons (make_unibyte_string ((char *) &desc_type, 4), elem);
	    break;
	  }

      if (err == noErr || desc_list->descriptorType == typeAEList)
	{
	  if (err != noErr)
	    elem = Qnil;	/* Don't skip elements in AEList.  */
	  else if (desc_list->descriptorType != typeAEList)
	    {
	      if (attribute_p)
		elem = Fcons (ae_attr_table[count-1].symbol, elem);
	      else
		{
		  keyword = EndianU32_NtoB (keyword);
		  elem = Fcons (make_unibyte_string ((char *) &keyword, 4),
				elem);
		}
	    }

	  result = Fcons (elem, result);
	}

      count--;
    }

  if (desc_list->descriptorType == typeAppleEvent && !attribute_p)
    {
      attribute_p = 1;
      count = sizeof (ae_attr_table) / sizeof (ae_attr_table[0]);
      goto again;
    }

  desc_type = EndianU32_NtoB (desc_list->descriptorType);
  return Fcons (make_unibyte_string ((char *) &desc_type, 4), result);
}

Lisp_Object
mac_aedesc_to_lisp (desc)
     const AEDesc *desc;
{
  OSErr err = noErr;
  DescType desc_type = desc->descriptorType;
  Lisp_Object result;

  switch (desc_type)
    {
    case typeNull:
      result = Qnil;
      break;

    case typeAEList:
    case typeAERecord:
    case typeAppleEvent:
      return mac_aelist_to_lisp (desc);
#if 0
      /* The following one is much simpler, but creates and disposes
	 of Apple event descriptors many times.  */
      {
	long count;
	Lisp_Object elem;
	AEKeyword keyword;
	AEDesc desc1;

	err = AECountItems (desc, &count);
	if (err != noErr)
	  break;
	result = Qnil;
	while (count > 0)
	  {
	    err = AEGetNthDesc (desc, count, typeWildCard, &keyword, &desc1);
	    if (err != noErr)
	      break;
	    elem = mac_aedesc_to_lisp (&desc1);
	    AEDisposeDesc (&desc1);
	    if (desc_type != typeAEList)
	      {
		keyword = EndianU32_NtoB (keyword);
		elem = Fcons (make_unibyte_string ((char *) &keyword, 4), elem);
	      }
	    result = Fcons (elem, result);
	    count--;
	  }
      }
#endif
      break;

    default:
      result = make_uninit_string (AEGetDescDataSize (desc));
      err = AEGetDescData (desc, SDATA (result), SBYTES (result));
      break;
    }

  if (err != noErr)
    return Qnil;

  desc_type = EndianU32_NtoB (desc_type);
  return Fcons (make_unibyte_string ((char *) &desc_type, 4), result);
}

static OSErr
mac_ae_put_lisp_1 (desc, keyword_or_index, obj, ancestors)
     AEDescList *desc;
     UInt32 keyword_or_index;
     Lisp_Object obj;
     struct bstree_node **ancestors;
{
  OSErr err;

  if (CONSP (obj) && STRINGP (XCAR (obj)) && SBYTES (XCAR (obj)) == 4)
    {
      DescType desc_type1 = EndianU32_BtoN (*((UInt32 *) SDATA (XCAR (obj))));
      Lisp_Object data = XCDR (obj), rest;
      AEDesc desc1;
      struct bstree_node **bstree_ref;

      switch (desc_type1)
	{
	case typeNull:
	case typeAppleEvent:
	  break;

	case typeAEList:
	case typeAERecord:
	  if (cdr_chain_length (data) < 0)
	    break;
	  bstree_ref = bstree_find (ancestors, obj);
	  if (*bstree_ref)
	    break;
	  else
	    {
	      struct bstree_node node;

	      node.obj = obj;
	      node.left = node.right = NULL;
	      *bstree_ref = &node;

	      err = AECreateList (NULL, 0, desc_type1 == typeAERecord, &desc1);
	      if (err == noErr)
		{
		  for (rest = data; CONSP (rest); rest = XCDR (rest))
		    {
		      UInt32 keyword_or_index1 = 0;
		      Lisp_Object elem = XCAR (rest);

		      if (desc_type1 == typeAERecord)
			{
			  if (CONSP (elem) && STRINGP (XCAR (elem))
			      && SBYTES (XCAR (elem)) == 4)
			    {
			      keyword_or_index1 =
				EndianU32_BtoN (*((UInt32 *)
						  SDATA (XCAR (elem))));
			      elem = XCDR (elem);
			    }
			  else
			    continue;
			}

		      err = mac_ae_put_lisp_1 (&desc1, keyword_or_index1, elem,
					       ancestors);
		      if (err != noErr)
			break;
		    }

		  if (err == noErr)
		    {
		      if (desc->descriptorType == typeAEList)
			err = AEPutDesc (desc, keyword_or_index, &desc1);
		      else
			err = AEPutParamDesc (desc, keyword_or_index, &desc1);
		    }

		  AEDisposeDesc (&desc1);
		}

	      *bstree_ref = NULL;
	    }
	  return err;

	default:
	  if (!STRINGP (data))
	    break;
	  if (desc->descriptorType == typeAEList)
	    err = AEPutPtr (desc, keyword_or_index, desc_type1,
			    SDATA (data), SBYTES (data));
	  else
	    err = AEPutParamPtr (desc, keyword_or_index, desc_type1,
				 SDATA (data), SBYTES (data));
	  return err;
	}
    }

  if (desc->descriptorType == typeAEList)
    err = AEPutPtr (desc, keyword_or_index, typeNull, NULL, 0);
  else
    err = AEPutParamPtr (desc, keyword_or_index, typeNull, NULL, 0);

  return err;
}

OSErr
mac_ae_put_lisp (desc, keyword_or_index, obj)
     AEDescList *desc;
     UInt32 keyword_or_index;
     Lisp_Object obj;
{
  struct bstree_node *root = NULL;

  if (!(desc->descriptorType == typeAppleEvent
	|| desc->descriptorType == typeAERecord
	|| desc->descriptorType == typeAEList))
    return errAEWrongDataType;

  return mac_ae_put_lisp_1 (desc, keyword_or_index, obj, &root);
}

OSErr
create_apple_event_from_lisp (apple_event, result)
     Lisp_Object apple_event;
     AppleEvent *result;
{
  OSErr err;

  if (!(CONSP (apple_event) && STRINGP (XCAR (apple_event))
	&& SBYTES (XCAR (apple_event)) == 4
	&& strcmp (SDATA (XCAR (apple_event)), "aevt") == 0
	&& cdr_chain_length (XCDR (apple_event)) >= 0))
    return errAEBuildSyntaxError;

  err = create_apple_event (0, 0, result);
  if (err == noErr)
    {
      Lisp_Object rest;

      for (rest = XCDR (apple_event); CONSP (rest); rest = XCDR (rest))
	{
	  Lisp_Object attr = XCAR (rest), name, type, data;
	  int i;

	  if (!(CONSP (attr) && SYMBOLP (XCAR (attr)) && CONSP (XCDR (attr))))
	    continue;
	  name = XCAR (attr);
	  type = XCAR (XCDR (attr));
	  data = XCDR (XCDR (attr));
	  if (!(STRINGP (type) && SBYTES (type) == 4))
	    continue;
	  for (i = 0; i < sizeof (ae_attr_table) / sizeof (ae_attr_table[0]);
	       i++)
	    if (EQ (name, ae_attr_table[i].symbol))
	      {
		DescType desc_type =
		  EndianU32_BtoN (*((UInt32 *) SDATA (type)));

		switch (desc_type)
		  {
		  case typeNull:
		    AEPutAttributePtr (result, ae_attr_table[i].keyword,
				       desc_type, NULL, 0);
		    break;

		  case typeAppleEvent:
		  case typeAEList:
		  case typeAERecord:
		    /* We assume there's no composite attribute value.  */
		    break;

		  default:
		    if (STRINGP (data))
		      AEPutAttributePtr (result, ae_attr_table[i].keyword,
					 desc_type,
					 SDATA (data), SBYTES (data));
		    break;
		  }
		break;
	      }
	}

      for (rest = XCDR (apple_event); CONSP (rest); rest = XCDR (rest))
	{
	  Lisp_Object param = XCAR (rest);

	  if (!(CONSP (param) && STRINGP (XCAR (param))
		&& SBYTES (XCAR (param)) == 4))
	    continue;
	  mac_ae_put_lisp (result,
			   EndianU32_BtoN (*((UInt32 *) SDATA (XCAR (param)))),
			   XCDR (param));
	}
    }

  return err;
}

static pascal OSErr
mac_coerce_file_name_ptr (type_code, data_ptr, data_size,
			  to_type, handler_refcon, result)
     DescType type_code;
     const void *data_ptr;
     Size data_size;
     DescType to_type;
     long handler_refcon;
     AEDesc *result;
{
  OSErr err;

  if (type_code == typeNull)
    err = errAECoercionFail;
  else if (type_code == to_type || to_type == typeWildCard)
    err = AECreateDesc (TYPE_FILE_NAME, data_ptr, data_size, result);
  else if (type_code == TYPE_FILE_NAME)
    /* Coercion from undecoded file name.  */
    {
      CFStringRef str;
      CFURLRef url = NULL;
      CFDataRef data = NULL;

      str = CFStringCreateWithBytes (NULL, data_ptr, data_size,
				     kCFStringEncodingUTF8, false);
      if (str)
	{
	  url = CFURLCreateWithFileSystemPath (NULL, str,
					       kCFURLPOSIXPathStyle, false);
	  CFRelease (str);
	}
      if (url)
	{
	  data = CFURLCreateData (NULL, url, kCFStringEncodingUTF8, true);
	  CFRelease (url);
	}
      if (data)
	{
	  err = AECoercePtr (typeFileURL, CFDataGetBytePtr (data),
			     CFDataGetLength (data), to_type, result);
	  CFRelease (data);
	}
      else
	err = memFullErr;

      if (err != noErr)
	{
	  /* Just to be paranoid ...  */
	  FSRef fref;
	  char *buf;

	  buf = xmalloc (data_size + 1);
	  memcpy (buf, data_ptr, data_size);
	  buf[data_size] = '\0';
	  err = FSPathMakeRef (buf, &fref, NULL);
	  xfree (buf);
	  if (err == noErr)
	    err = AECoercePtr (typeFSRef, &fref, sizeof (FSRef),
			       to_type, result);
	}
    }
  else if (to_type == TYPE_FILE_NAME)
    /* Coercion to undecoded file name.  */
    {
      CFURLRef url = NULL;
      CFStringRef str = NULL;
      CFDataRef data = NULL;

      if (type_code == typeFileURL)
	{
	  url = CFURLCreateWithBytes (NULL, data_ptr, data_size,
				      kCFStringEncodingUTF8, NULL);
	  err = noErr;
	}
      else
	{
	  AEDesc desc;
	  Size size;
	  char *buf;

	  err = AECoercePtr (type_code, data_ptr, data_size,
			     typeFileURL, &desc);
	  if (err == noErr)
	    {
	      size = AEGetDescDataSize (&desc);
	      buf = xmalloc (size);
	      err = AEGetDescData (&desc, buf, size);
	      if (err == noErr)
		url = CFURLCreateWithBytes (NULL, buf, size,
					    kCFStringEncodingUTF8, NULL);
	      xfree (buf);
	      AEDisposeDesc (&desc);
	    }
	}
      if (url)
	{
	  str = CFURLCopyFileSystemPath (url, kCFURLPOSIXPathStyle);
	  CFRelease (url);
	}
      if (str)
	{
	  data = CFStringCreateExternalRepresentation (NULL, str,
						       kCFStringEncodingUTF8,
						       '\0');
	  CFRelease (str);
	}
      if (data)
	{
	  err = AECreateDesc (TYPE_FILE_NAME, CFDataGetBytePtr (data),
			      CFDataGetLength (data), result);
	  CFRelease (data);
	}

      if (err != noErr)
	{
	  /* Coercion from typeAlias to typeFileURL fails on Mac OS X
	     10.2.  In such cases, try typeFSRef as a target type.  */
	  char file_name[MAXPATHLEN];

	  if (type_code == typeFSRef && data_size == sizeof (FSRef))
	    err = FSRefMakePath (data_ptr, file_name, sizeof (file_name));
	  else
	    {
	      AEDesc desc;
	      FSRef fref;

	      err = AECoercePtr (type_code, data_ptr, data_size,
				 typeFSRef, &desc);
	      if (err == noErr)
		{
		  err = AEGetDescData (&desc, &fref, sizeof (FSRef));
		  AEDisposeDesc (&desc);
		}
	      if (err == noErr)
		err = FSRefMakePath (&fref, file_name, sizeof (file_name));
	    }
	  if (err == noErr)
	    err = AECreateDesc (TYPE_FILE_NAME, file_name,
				strlen (file_name), result);
	}
    }
  else
    abort ();

  if (err != noErr)
    return errAECoercionFail;
  return noErr;
}

static pascal OSErr
mac_coerce_file_name_desc (from_desc, to_type, handler_refcon, result)
     const AEDesc *from_desc;
     DescType to_type;
     long handler_refcon;
     AEDesc *result;
{
  OSErr err = noErr;
  DescType from_type = from_desc->descriptorType;

  if (from_type == typeNull)
    err = errAECoercionFail;
  else if (from_type == to_type || to_type == typeWildCard)
    err = AEDuplicateDesc (from_desc, result);
  else
    {
      char *data_ptr;
      Size data_size;

      data_size = AEGetDescDataSize (from_desc);
      data_ptr = xmalloc (data_size);
      err = AEGetDescData (from_desc, data_ptr, data_size);
      if (err == noErr)
	err = mac_coerce_file_name_ptr (from_type, data_ptr,
					data_size, to_type,
					handler_refcon, result);
      xfree (data_ptr);
    }

  if (err != noErr)
    return errAECoercionFail;
  return noErr;
}

OSErr
init_coercion_handler ()
{
  OSErr err;

  static AECoercePtrUPP coerce_file_name_ptrUPP = NULL;
  static AECoerceDescUPP coerce_file_name_descUPP = NULL;

  if (coerce_file_name_ptrUPP == NULL)
    {
      coerce_file_name_ptrUPP = NewAECoercePtrUPP (mac_coerce_file_name_ptr);
      coerce_file_name_descUPP = NewAECoerceDescUPP (mac_coerce_file_name_desc);
    }

  err = AEInstallCoercionHandler (TYPE_FILE_NAME, typeWildCard,
				  (AECoercionHandlerUPP)
				  coerce_file_name_ptrUPP, 0, false, false);
  if (err == noErr)
    err = AEInstallCoercionHandler (typeWildCard, TYPE_FILE_NAME,
				    (AECoercionHandlerUPP)
				    coerce_file_name_ptrUPP, 0, false, false);
  if (err == noErr)
    err = AEInstallCoercionHandler (TYPE_FILE_NAME, typeWildCard,
				    coerce_file_name_descUPP, 0, true, false);
  if (err == noErr)
    err = AEInstallCoercionHandler (typeWildCard, TYPE_FILE_NAME,
				    coerce_file_name_descUPP, 0, true, false);
  return err;
}

OSErr
create_apple_event (class, id, result)
     AEEventClass class;
     AEEventID id;
     AppleEvent *result;
{
  OSErr err;
  static const ProcessSerialNumber psn = {0, kCurrentProcess};
  AEAddressDesc address_desc;

  err = AECreateDesc (typeProcessSerialNumber, &psn,
		      sizeof (ProcessSerialNumber), &address_desc);
  if (err == noErr)
    {
      err = AECreateAppleEvent (class, id,
				&address_desc, /* NULL is not allowed
						  on Mac OS Classic. */
				kAutoGenerateReturnID,
				kAnyTransactionID, result);
      AEDisposeDesc (&address_desc);
    }

  return err;
}

Lisp_Object
mac_event_parameters_to_lisp (event, num_params, names, types)
     EventRef event;
     UInt32 num_params;
     const EventParamName *names;
     const EventParamType *types;
{
  OSStatus err;
  Lisp_Object result = Qnil;
  UInt32 i;
  ByteCount size;
  CFStringRef string;
  CFDataRef data;
  char *buf = NULL;

  for (i = 0; i < num_params; i++)
    {
      EventParamName name = names[i];
      EventParamType type = types[i];

      switch (type)
	{
	case typeCFStringRef:
	  err = GetEventParameter (event, name, typeCFStringRef, NULL,
				   sizeof (CFStringRef), NULL, &string);
	  if (err != noErr)
	    break;
	  data = CFStringCreateExternalRepresentation (NULL, string,
						       kCFStringEncodingUTF8,
						       '?');
	  if (data == NULL)
	    break;
	  name = EndianU32_NtoB (name);
	  type = EndianU32_NtoB (typeUTF8Text);
	  result =
	    Fcons (Fcons (make_unibyte_string ((char *) &name, 4),
			  Fcons (make_unibyte_string ((char *) &type, 4),
				 make_unibyte_string (CFDataGetBytePtr (data),
						      CFDataGetLength (data)))),
		   result);
	  CFRelease (data);
	  break;

	default:
	  err = GetEventParameter (event, name, type, NULL, 0, &size, NULL);
	  if (err != noErr)
	    break;
	  buf = xrealloc (buf, size);
	  err = GetEventParameter (event, name, type, NULL, size, NULL, buf);
	  if (err == noErr)
	    {
	      name = EndianU32_NtoB (name);
	      type = EndianU32_NtoB (type);
	      result =
		Fcons (Fcons (make_unibyte_string ((char *) &name, 4),
			      Fcons (make_unibyte_string ((char *) &type, 4),
				     make_unibyte_string (buf, size))),
		       result);
	    }
	  break;
	}
    }
  xfree (buf);

  return result;
}


/***********************************************************************
	 Conversion between Lisp and Core Foundation objects
 ***********************************************************************/

Lisp_Object Qstring, Qnumber, Qboolean, Qdate, Qarray, Qdictionary;
Lisp_Object Qrange, Qpoint;
extern Lisp_Object Qdata;
static Lisp_Object Qdescription;

struct cfdict_context
{
  Lisp_Object *result;
  int flags, hash_bound;
};

/* C string to CFString.  */

CFStringRef
cfstring_create_with_utf8_cstring (c_str)
     const char *c_str;
{
  CFStringRef str;

  str = CFStringCreateWithCString (NULL, c_str, kCFStringEncodingUTF8);
  if (str == NULL)
    /* Failed to interpret as UTF 8.  Fall back on Mac Roman.  */
    str = CFStringCreateWithCString (NULL, c_str, kCFStringEncodingMacRoman);

  return str;
}


/* Lisp string containing a UTF-8 byte sequence to CFString.  Unlike
   cfstring_create_with_utf8_cstring, this function preserves NUL
   characters.  */

CFStringRef
cfstring_create_with_string_noencode (s)
     Lisp_Object s;
{
  CFStringRef string = CFStringCreateWithBytes (NULL, SDATA (s), SBYTES (s),
						kCFStringEncodingUTF8, false);

  if (string == NULL)
    /* Failed to interpret as UTF 8.  Fall back on Mac Roman.  */
    string = CFStringCreateWithBytes (NULL, SDATA (s), SBYTES (s),
				      kCFStringEncodingMacRoman, false);

  return string;
}

/* Lisp string to CFString.  */

CFStringRef
cfstring_create_with_string (s)
     Lisp_Object s;
{
  if (STRING_MULTIBYTE (s))
    {
      char *p, *end = SDATA (s) + SBYTES (s);

      for (p = SDATA (s); p < end; p++)
	if (!isascii (*p))
	  {
	    s = ENCODE_UTF_8 (s);
	    break;
	  }
      return cfstring_create_with_string_noencode (s);
    }
  else
    return CFStringCreateWithBytes (NULL, SDATA (s), SBYTES (s),
				    kCFStringEncodingMacRoman, false);
}


/* From CFData to a lisp string.  Always returns a unibyte string.  */

Lisp_Object
cfdata_to_lisp (data)
     CFDataRef data;
{
  CFIndex len = CFDataGetLength (data);
  Lisp_Object result = make_uninit_string (len);

  CFDataGetBytes (data, CFRangeMake (0, len), SDATA (result));

  return result;
}


/* From CFString to a lisp string.  Returns a unibyte string
   containing a UTF-8 byte sequence.  */

Lisp_Object
cfstring_to_lisp_nodecode (string)
     CFStringRef string;
{
  Lisp_Object result = Qnil;
  CFDataRef data;
  const char *s = CFStringGetCStringPtr (string, kCFStringEncodingUTF8);

  if (s)
    {
      CFIndex i, length = CFStringGetLength (string);

      for (i = 0; i < length; i++)
	if (CFStringGetCharacterAtIndex (string, i) == 0)
	  break;

      if (i == length)
	return make_unibyte_string (s, strlen (s));
    }

  data = CFStringCreateExternalRepresentation (NULL, string,
					       kCFStringEncodingUTF8, '?');
  if (data)
    {
      result = cfdata_to_lisp (data);
      CFRelease (data);
    }

  return result;
}


/* From CFString to a lisp string.  Never returns a unibyte string
   (even if it only contains ASCII characters).
   This may cause GC during code conversion. */

Lisp_Object
cfstring_to_lisp (string)
     CFStringRef string;
{
  Lisp_Object result = cfstring_to_lisp_nodecode (string);

  if (!NILP (result))
    {
      result = code_convert_string_norecord (result, Qutf_8, 0);
      /* This may be superfluous.  Just to make sure that the result
	 is a multibyte string.  */
      result = string_to_multibyte (result);
    }

  return result;
}


/* From CFString to a lisp string.  Returns a unibyte string
   containing a UTF-16 byte sequence in native byte order, no BOM.  */

Lisp_Object
cfstring_to_lisp_utf_16 (string)
     CFStringRef string;
{
  Lisp_Object result = Qnil;
  CFIndex len, buf_len;

  len = CFStringGetLength (string);
  if (CFStringGetBytes (string, CFRangeMake (0, len), kCFStringEncodingUnicode,
			0, false, NULL, 0, &buf_len) == len)
    {
      result = make_uninit_string (buf_len);
      CFStringGetBytes (string, CFRangeMake (0, len), kCFStringEncodingUnicode,
			0, false, SDATA (result), buf_len, NULL);
    }

  return result;
}


/* CFNumber to a lisp integer, float, or string in decimal.  */

Lisp_Object
cfnumber_to_lisp (number)
     CFNumberRef number;
{
  Lisp_Object result = Qnil;
#if BITS_PER_EMACS_INT > 32
  SInt64 int_val;
  CFNumberType emacs_int_type = kCFNumberSInt64Type;
#else
  SInt32 int_val;
  CFNumberType emacs_int_type = kCFNumberSInt32Type;
#endif
  double float_val;

  if (CFNumberGetValue (number, emacs_int_type, &int_val)
      && !FIXNUM_OVERFLOW_P (int_val))
    result = make_number (int_val);
  else if (CFNumberGetValue (number, kCFNumberDoubleType, &float_val))
    result = make_float (float_val);
  else
    {
      CFStringRef string = CFStringCreateWithFormat (NULL, NULL,
						     CFSTR ("%@"), number);
      if (string)
	{
	  result = cfstring_to_lisp_nodecode (string);
	  CFRelease (string);
	}
    }
  return result;
}


/* CFDate to a list of three integers as in a return value of
   `current-time'.  */

Lisp_Object
cfdate_to_lisp (date)
     CFDateRef date;
{
  CFTimeInterval sec;
  int high, low, microsec;

  sec = CFDateGetAbsoluteTime (date) + kCFAbsoluteTimeIntervalSince1970;
  high = sec / 65536.0;
  low = sec - high * 65536.0;
  microsec = (sec - floor (sec)) * 1000000.0;

  return list3 (make_number (high), make_number (low), make_number (microsec));
}


/* CFBoolean to a lisp symbol, `t' or `nil'.  */

Lisp_Object
cfboolean_to_lisp (boolean)
     CFBooleanRef boolean;
{
  return CFBooleanGetValue (boolean) ? Qt : Qnil;
}


/* Any Core Foundation object to a (lengthy) lisp string.  */

Lisp_Object
cfobject_desc_to_lisp (object)
     CFTypeRef object;
{
  Lisp_Object result = Qnil;
  CFStringRef desc = CFCopyDescription (object);

  if (desc)
    {
      result = cfstring_to_lisp (desc);
      CFRelease (desc);
    }

  return result;
}


/* Callback functions for cfobject_to_lisp.  */

static void
cfdictionary_add_to_list (key, value, context)
     const void *key;
     const void *value;
     void *context;
{
  struct cfdict_context *cxt = (struct cfdict_context *)context;
  Lisp_Object lisp_key;

  if (CFGetTypeID (key) != CFStringGetTypeID ())
    lisp_key = cfobject_to_lisp (key, cxt->flags, cxt->hash_bound);
  else if (cxt->flags & CFOBJECT_TO_LISP_DONT_DECODE_DICTIONARY_KEY)
    lisp_key = cfstring_to_lisp_nodecode (key);
  else
    lisp_key = cfstring_to_lisp (key);

  *cxt->result =
    Fcons (Fcons (lisp_key,
		  cfobject_to_lisp (value, cxt->flags, cxt->hash_bound)),
	   *cxt->result);
}

static void
cfdictionary_puthash (key, value, context)
     const void *key;
     const void *value;
     void *context;
{
  Lisp_Object lisp_key;
  struct cfdict_context *cxt = (struct cfdict_context *)context;
  struct Lisp_Hash_Table *h = XHASH_TABLE (*(cxt->result));
  unsigned hash_code;

  if (CFGetTypeID (key) != CFStringGetTypeID ())
    lisp_key = cfobject_to_lisp (key, cxt->flags, cxt->hash_bound);
  else if (cxt->flags & CFOBJECT_TO_LISP_DONT_DECODE_DICTIONARY_KEY)
    lisp_key = cfstring_to_lisp_nodecode (key);
  else
    lisp_key = cfstring_to_lisp (key);

  hash_lookup (h, lisp_key, &hash_code);
  hash_put (h, lisp_key,
	    cfobject_to_lisp (value, cxt->flags, cxt->hash_bound),
	    hash_code);
}


/* Convert Core Foundation Object OBJ to a Lisp object.

   FLAGS is bitwise-or of some of the following flags.
   If CFOBJECT_TO_LISP_WITH_TAG is set, a symbol that represents the
   type of the original Core Foundation object is prepended.
   If CFOBJECT_TO_LISP_DONT_DECODE_STRING is set, CFStrings (except
   dictionary keys) are not decoded and the resulting Lisp objects are
   unibyte strings as UTF-8 byte sequences.
   If CFOBJECT_TO_LISP_DONT_DECODE_DICTIONARY_KEY is set, dictionary
   key CFStrings are not decoded.

   HASH_BOUND specifies which kinds of the lisp objects, alists or
   hash tables, are used as the targets of the conversion from
   CFDictionary.  If HASH_BOUND is negative, always generate alists.
   If HASH_BOUND >= 0, generate an alist if the number of keys in the
   dictionary is smaller than HASH_BOUND, and a hash table
   otherwise.  */

Lisp_Object
cfobject_to_lisp (obj, flags, hash_bound)
     CFTypeRef obj;
     int flags, hash_bound;
{
  CFTypeID type_id = CFGetTypeID (obj);
  Lisp_Object tag = Qnil, result = Qnil;
  struct gcpro gcpro1, gcpro2;

  GCPRO2 (tag, result);

  if (type_id == CFStringGetTypeID ())
    {
      tag = Qstring;
      if (flags & CFOBJECT_TO_LISP_DONT_DECODE_STRING)
	result = cfstring_to_lisp_nodecode (obj);
      else
	result = cfstring_to_lisp (obj);
    }
  else if (type_id == CFNumberGetTypeID ())
    {
      tag = Qnumber;
      result = cfnumber_to_lisp (obj);
    }
  else if (type_id == CFBooleanGetTypeID ())
    {
      tag = Qboolean;
      result = cfboolean_to_lisp (obj);
    }
  else if (type_id == CFDateGetTypeID ())
    {
      tag = Qdate;
      result = cfdate_to_lisp (obj);
    }
  else if (type_id == CFDataGetTypeID ())
    {
      tag = Qdata;
      result = cfdata_to_lisp (obj);
    }
  else if (type_id == CFArrayGetTypeID ())
    {
      CFIndex index, count = CFArrayGetCount (obj);

      tag = Qarray;
      result = Fmake_vector (make_number (count), Qnil);
      for (index = 0; index < count; index++)
	XVECTOR (result)->contents[index] =
	  cfobject_to_lisp (CFArrayGetValueAtIndex (obj, index),
			    flags, hash_bound);
    }
  else if (type_id == CFDictionaryGetTypeID ())
    {
      struct cfdict_context context;
      CFIndex count = CFDictionaryGetCount (obj);

      tag = Qdictionary;
      context.result  = &result;
      context.flags = flags;
      context.hash_bound = hash_bound;
      if (hash_bound < 0 || count < hash_bound)
	{
	  result = Qnil;
	  CFDictionaryApplyFunction (obj, cfdictionary_add_to_list,
				     &context);
	}
      else
	{
	  result = make_hash_table (Qequal,
				    make_number (count),
				    make_float (DEFAULT_REHASH_SIZE),
				    make_float (DEFAULT_REHASH_THRESHOLD),
				    Qnil, Qnil, Qnil);
	  CFDictionaryApplyFunction (obj, cfdictionary_puthash,
				     &context);
	}
    }
  else
    {
      Lisp_Object tag_result = mac_nsvalue_to_lisp (obj);

      if (CONSP (tag_result))
	{
	  tag = XCAR (tag_result);
	  result = XCDR (tag_result);
	}
      else
	{
	  CFStringRef desc = CFCopyDescription (obj);

	  tag = Qdescription;
	  if (desc)
	    {
	      if (flags & CFOBJECT_TO_LISP_DONT_DECODE_STRING)
		result = cfstring_to_lisp_nodecode (desc);
	      else
		result = cfstring_to_lisp (desc);

	      CFRelease (desc);
	    }
	}
    }

  UNGCPRO;

  if (flags & CFOBJECT_TO_LISP_WITH_TAG)
    result = Fcons (tag, result);

  return result;
}

/* Convert CFPropertyList PLIST to a lisp object.  If WITH_TAG is
   non-zero, a symbol that represents the type of the original Core
   Foundation object is prepended.  HASH_BOUND specifies which kinds
   of the lisp objects, alists or hash tables, are used as the targets
   of the conversion from CFDictionary.  If HASH_BOUND is negative,
   always generate alists.  If HASH_BOUND >= 0, generate an alist if
   the number of keys in the dictionary is smaller than HASH_BOUND,
   and a hash table otherwise.  */

Lisp_Object
cfproperty_list_to_lisp (plist, with_tag, hash_bound)
     CFPropertyListRef plist;
     int with_tag, hash_bound;
{
  return cfobject_to_lisp (plist, with_tag ? CFOBJECT_TO_LISP_WITH_TAG : 0,
			   hash_bound);
}

static CFPropertyListRef
cfproperty_list_create_with_lisp_1 (obj, ancestors)
     Lisp_Object obj;
     struct bstree_node **ancestors;
{
  CFPropertyListRef result = NULL;
  Lisp_Object type, data;
  struct bstree_node **bstree_ref;

  if (!CONSP (obj))
    return NULL;

  type = XCAR (obj);
  data = XCDR (obj);
  if (EQ (type, Qstring))
    {
      if (STRINGP (data))
	result = cfstring_create_with_string (data);
    }
  else if (EQ (type, Qnumber))
    {
      if (INTEGERP (data))
	{
	  long value = XINT (data);

	  result = CFNumberCreate (NULL, kCFNumberLongType, &value);
	}
      else if (FLOATP (data))
	{
	  double value = XFLOAT_DATA (data);

	  result = CFNumberCreate (NULL, kCFNumberDoubleType, &value);
	}
      else if (STRINGP (data))
	{
	  SInt64 value = strtoll (SDATA (data), NULL, 0);

	  result = CFNumberCreate (NULL, kCFNumberSInt64Type, &value);
	}
    }
  else if (EQ (type, Qboolean))
    {
      if (NILP (data))
	result = kCFBooleanFalse;
      else if (EQ (data, Qt))
	result = kCFBooleanTrue;
    }
  else if (EQ (type, Qdate))
    {
      if (CONSP (data) && INTEGERP (XCAR (data))
	  && CONSP (XCDR (data)) && INTEGERP (XCAR (XCDR (data)))
	  && CONSP (XCDR (XCDR (data)))
	  && INTEGERP (XCAR (XCDR (XCDR (data)))))
	{
	  CFAbsoluteTime at;

	  at = (XINT (XCAR (data)) * 65536.0 + XINT (XCAR (XCDR (data)))
		+ XINT (XCAR (XCDR (XCDR (data)))) * 0.000001
		- kCFAbsoluteTimeIntervalSince1970);
	  result = CFDateCreate (NULL, at);
	}
    }
  else if (EQ (type, Qdata))
    {
      if (STRINGP (data))
	result = CFDataCreate (NULL, SDATA (data), SBYTES (data));
    }
  /* Recursive cases follow.  */
  else if ((bstree_ref = bstree_find (ancestors, obj),
	    *bstree_ref == NULL))
    {
      struct bstree_node node;

      node.obj = obj;
      node.left = node.right = NULL;
      *bstree_ref = &node;

      if (EQ (type, Qarray))
	{
	  if (VECTORP (data))
	    {
	      EMACS_INT size = ASIZE (data);
	      CFMutableArrayRef array =
		CFArrayCreateMutable (NULL, size, &kCFTypeArrayCallBacks);

	      if (array)
		{
		  EMACS_INT i;

		  for (i = 0; i < size; i++)
		    {
		      CFPropertyListRef value =
			cfproperty_list_create_with_lisp_1 (AREF (data, i),
							    ancestors);

		      if (value)
			{
			  CFArrayAppendValue (array, value);
			  CFRelease (value);
			}
		      else
			break;
		    }
		  if (i < size)
		    {
		      CFRelease (array);
		      array = NULL;
		    }
		}
	      result = array;
	    }
	}
      else if (EQ (type, Qdictionary))
	{
	  CFMutableDictionaryRef dictionary = NULL;

	  if (CONSP (data) || NILP (data))
	    {
	      EMACS_INT size = cdr_chain_length (data);

	      if (size >= 0)
		dictionary =
		  CFDictionaryCreateMutable (NULL, size,
					     &kCFTypeDictionaryKeyCallBacks,
					     &kCFTypeDictionaryValueCallBacks);
	      if (dictionary)
		{
		  for (; CONSP (data); data = XCDR (data))
		    {
		      CFPropertyListRef value = NULL;

		      if (CONSP (XCAR (data)) && STRINGP (XCAR (XCAR (data))))
			{
			  CFStringRef key =
			    cfstring_create_with_string (XCAR (XCAR (data)));

			  if (key)
			    {
			      value = cfproperty_list_create_with_lisp_1 (XCDR (XCAR (data)),
									  ancestors);
			      if (value)
				{
				  CFDictionaryAddValue (dictionary, key, value);
				  CFRelease (value);
				}
			      CFRelease (key);
			    }
			}
		      if (value == NULL)
			break;
		    }
		  if (!NILP (data))
		    {
		      CFRelease (dictionary);
		      dictionary = NULL;
		    }
		}
	    }
	  else if (HASH_TABLE_P (data))
	    {
	      struct Lisp_Hash_Table *h = XHASH_TABLE (data);

	      dictionary =
		CFDictionaryCreateMutable (NULL,
					   XINT (Fhash_table_count (data)),
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	      if (dictionary)
		{
		  int i, size = HASH_TABLE_SIZE (h);

		  for (i = 0; i < size; ++i)
		    if (!NILP (HASH_HASH (h, i)))
		      {
			CFPropertyListRef value = NULL;

			if (STRINGP (HASH_KEY (h, i)))
			  {
			    CFStringRef key =
			      cfstring_create_with_string (HASH_KEY (h, i));

			    if (key)
			      {
				value = cfproperty_list_create_with_lisp_1 (HASH_VALUE (h, i),
									    ancestors);
				if (value)
				  {
				    CFDictionaryAddValue (dictionary,
							  key, value);
				    CFRelease (value);
				  }
				CFRelease (key);
			      }
			  }
			if (value == NULL)
			  break;
		      }
		  if (i < size)
		    {
		      CFRelease (dictionary);
		      dictionary = NULL;
		    }
		}
	    }
	  result = dictionary;
	}

      *bstree_ref = NULL;
    }

  return result;
}

/* Create CFPropertyList from a Lisp object OBJ, which must be a form
   of a return value of cfproperty_list_to_lisp with with_tag set.  */

CFPropertyListRef
cfproperty_list_create_with_lisp (obj)
     Lisp_Object obj;
{
  struct bstree_node *root = NULL;

  return cfproperty_list_create_with_lisp_1 (obj, &root);
}

/* Convert CFPropertyList PLIST to a unibyte string in FORMAT, which
   is either kCFPropertyListXMLFormat_v1_0 or
   kCFPropertyListBinaryFormat_v1_0.  Return nil if an error has
   occurred.  */

Lisp_Object
cfproperty_list_to_string (plist, format)
     CFPropertyListRef plist;
     CFPropertyListFormat format;
{
  Lisp_Object result = Qnil;
  CFDataRef data = NULL;

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1060
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
  if (CFPropertyListCreateData != NULL)
#endif
    {
      data = CFPropertyListCreateData (NULL, plist, format, 0, NULL);
    }
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
  else				/* CFPropertyListCreateData == NULL */
#endif
#endif	/* MAC_OS_X_VERSION_MAX_ALLOWED >= 1060 */
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
    {
      CFWriteStreamRef stream;

      switch (format)
	{
	case kCFPropertyListXMLFormat_v1_0:
	  data = CFPropertyListCreateXMLData (NULL, plist);
	  break;

	case kCFPropertyListBinaryFormat_v1_0:
	  stream = CFWriteStreamCreateWithAllocatedBuffers (NULL, NULL);

	  if (stream)
	    {
	      CFWriteStreamOpen (stream);
	      if (CFPropertyListWriteToStream (plist, stream, format, NULL) > 0)
		data = CFWriteStreamCopyProperty (stream,
						  kCFStreamPropertyDataWritten);
	      CFWriteStreamClose (stream);
	      CFRelease (stream);
	    }
	  break;
	}
    }
#endif
  if (data)
    {
      result = cfdata_to_lisp (data);
      CFRelease (data);
    }

  return result;
}

/* Create CFPropertyList from a Lisp string in either
   kCFPropertyListXMLFormat_v1_0 or kCFPropertyListBinaryFormat_v1_0.
   Return NULL if an error has occurred.  */

CFPropertyListRef
cfproperty_list_create_with_string (string)
     Lisp_Object string;
{
  CFPropertyListRef result = NULL;
  CFDataRef data;

  string = Fstring_as_unibyte (string);
  data = CFDataCreateWithBytesNoCopy (NULL, SDATA (string), SBYTES (string),
				      kCFAllocatorNull);
  if (data)
    {
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1060
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
      if (CFPropertyListCreateWithData != NULL)
#endif
	{
	  result = CFPropertyListCreateWithData (NULL, data,
						 kCFPropertyListImmutable,
						 NULL, NULL);
	}
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
      else		    /* CFPropertyListCreateWithData == NULL */
#endif
#endif	/* MAC_OS_X_VERSION_MAX_ALLOWED >= 1060 */
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
	{
	  result = CFPropertyListCreateFromXMLData (NULL, data,
						    kCFPropertyListImmutable,
						    NULL);
	}
#endif
      CFRelease (data);
    }

  return result;
}


/***********************************************************************
		 Emulation of the X Resource Manager
 ***********************************************************************/

/* Parser functions for resource lines.  Each function takes an
   address of a variable whose value points to the head of a string.
   The value will be advanced so that it points to the next character
   of the parsed part when the function returns.

   A resource name such as "Emacs*font" is parsed into a non-empty
   list called `quarks'.  Each element is either a Lisp string that
   represents a concrete component, a Lisp symbol LOOSE_BINDING
   (actually Qlambda) that represents any number (>=0) of intervening
   components, or a Lisp symbol SINGLE_COMPONENT (actually Qquote)
   that represents as any single component.  */

#define P (*p)

#define LOOSE_BINDING    Qlambda /* '*' ("L"oose) */
#define SINGLE_COMPONENT Qquote	 /* '?' ("Q"uestion) */

static void
skip_white_space (p)
     const char **p;
{
  /* WhiteSpace = {<space> | <horizontal tab>} */
  while (*P == ' ' || *P == '\t')
    P++;
}

static int
parse_comment (p)
     const char **p;
{
  /* Comment = "!" {<any character except null or newline>} */
  if (*P == '!')
    {
      P++;
      while (*P)
	if (*P++ == '\n')
	  break;
      return 1;
    }
  else
    return 0;
}

/* Don't interpret filename.  Just skip until the newline.  */
static int
parse_include_file (p)
     const char **p;
{
  /* IncludeFile = "#" WhiteSpace "include" WhiteSpace FileName WhiteSpace */
  if (*P == '#')
    {
      P++;
      while (*P)
	if (*P++ == '\n')
	  break;
      return 1;
    }
  else
    return 0;
}

static char
parse_binding (p)
     const char **p;
{
  /* Binding = "." | "*"  */
  if (*P == '.' || *P == '*')
    {
      char binding = *P++;

      while (*P == '.' || *P == '*')
	if (*P++ == '*')
	  binding = '*';
      return binding;
    }
  else
    return '\0';
}

static Lisp_Object
parse_component (p)
     const char **p;
{
  /*  Component = "?" | ComponentName
      ComponentName = NameChar {NameChar}
      NameChar = "a"-"z" | "A"-"Z" | "0"-"9" | "_" | "-" */
  if (*P == '?')
    {
      P++;
      return SINGLE_COMPONENT;
    }
  else if (isalnum (*P) || *P == '_' || *P == '-')
    {
      const char *start = P++;

      while (isalnum (*P) || *P == '_' || *P == '-')
	P++;

      return make_unibyte_string (start, P - start);
    }
  else
    return Qnil;
}

static Lisp_Object
parse_resource_name (p)
     const char **p;
{
  Lisp_Object result = Qnil, component;
  char binding;

  /* ResourceName = [Binding] {Component Binding} ComponentName */
  if (parse_binding (p) == '*')
    result = Fcons (LOOSE_BINDING, result);

  component = parse_component (p);
  if (NILP (component))
    return Qnil;

  result = Fcons (component, result);
  while ((binding = parse_binding (p)) != '\0')
    {
      if (binding == '*')
	result = Fcons (LOOSE_BINDING, result);
      component = parse_component (p);
      if (NILP (component))
	return Qnil;
      else
	result = Fcons (component, result);
    }

  /* The final component should not be '?'.  */
  if (EQ (component, SINGLE_COMPONENT))
    return Qnil;

  return Fnreverse (result);
}

static Lisp_Object
parse_value (p)
     const char **p;
{
  char *q, *buf;
  Lisp_Object seq = Qnil, result;
  int buf_len, total_len = 0, len, continue_p;

  q = strchr (P, '\n');
  buf_len = q ? q - P : strlen (P);
  buf = xmalloc (buf_len);

  while (1)
    {
      q = buf;
      continue_p = 0;
      while (*P)
	{
	  if (*P == '\n')
	    {
	      P++;
	      break;
	    }
	  else if (*P == '\\')
	    {
	      P++;
	      if (*P == '\0')
		break;
	      else if (*P == '\n')
		{
		  P++;
		  continue_p = 1;
		  break;
		}
	      else if (*P == 'n')
		{
		  *q++ = '\n';
		  P++;
		}
	      else if ('0' <= P[0] && P[0] <= '7'
		       && '0' <= P[1] && P[1] <= '7'
		       && '0' <= P[2] && P[2] <= '7')
		{
		  *q++ = ((P[0] - '0') << 6) + ((P[1] - '0') << 3) + (P[2] - '0');
		  P += 3;
		}
	      else
		*q++ = *P++;
	    }
	  else
	    *q++ = *P++;
	}
      len = q - buf;
      seq = Fcons (make_unibyte_string (buf, len), seq);
      total_len += len;

      if (continue_p)
	{
	  q = strchr (P, '\n');
	  len = q ? q - P : strlen (P);
	  if (len > buf_len)
	    {
	      xfree (buf);
	      buf_len = len;
	      buf = xmalloc (buf_len);
	    }
	}
      else
	break;
    }
  xfree (buf);

  if (SBYTES (XCAR (seq)) == total_len)
    return make_string (SDATA (XCAR (seq)), total_len);
  else
    {
      buf = xmalloc (total_len);
      q = buf + total_len;
      for (; CONSP (seq); seq = XCDR (seq))
	{
	  len = SBYTES (XCAR (seq));
	  q -= len;
	  memcpy (q, SDATA (XCAR (seq)), len);
	}
      result = make_string (buf, total_len);
      xfree (buf);
      return result;
    }
}

static Lisp_Object
parse_resource_line (p)
     const char **p;
{
  Lisp_Object quarks, value;

  /* ResourceLine = Comment | IncludeFile | ResourceSpec | <empty line> */
  if (parse_comment (p) || parse_include_file (p))
    return Qnil;

  /* ResourceSpec = WhiteSpace ResourceName WhiteSpace ":" WhiteSpace Value */
  skip_white_space (p);
  quarks = parse_resource_name (p);
  if (NILP (quarks))
    goto cleanup;
  skip_white_space (p);
  if (*P != ':')
    goto cleanup;
  P++;
  skip_white_space (p);
  value = parse_value (p);
  return Fcons (quarks, value);

 cleanup:
  /* Skip the remaining data as a dummy value.  */
  parse_value (p);
  return Qnil;
}

#undef P

/* Equivalents of X Resource Manager functions.

   An X Resource Database acts as a collection of resource names and
   associated values.  It is implemented as a trie on quarks.  Namely,
   each edge is labeled by either a string, LOOSE_BINDING, or
   SINGLE_COMPONENT.  Each node has a node id, which is a unique
   nonnegative integer, and the root node id is 0.  A database is
   implemented as a hash table that maps a pair (SRC-NODE-ID .
   EDGE-LABEL) to DEST-NODE-ID.  It also holds a maximum node id used
   in the table as a value for HASHKEY_MAX_NID.  A value associated to
   a node is recorded as a value for the node id.

   A database also has a cache for past queries as a value for
   HASHKEY_QUERY_CACHE.  It is another hash table that maps
   "NAME-STRING\0CLASS-STRING" to the result of the query.  */

#define HASHKEY_MAX_NID (make_number (0))
#define HASHKEY_QUERY_CACHE (make_number (-1))

static XrmDatabase
xrm_create_database ()
{
  XrmDatabase database;

  database = make_hash_table (Qequal, make_number (DEFAULT_HASH_SIZE),
			      make_float (DEFAULT_REHASH_SIZE),
			      make_float (DEFAULT_REHASH_THRESHOLD),
			      Qnil, Qnil, Qnil);
  Fputhash (HASHKEY_MAX_NID, make_number (0), database);
  Fputhash (HASHKEY_QUERY_CACHE, Qnil, database);

  return database;
}

static void
xrm_q_put_resource (database, quarks, value)
     XrmDatabase database;
     Lisp_Object quarks, value;
{
  struct Lisp_Hash_Table *h = XHASH_TABLE (database);
  unsigned hash_code;
  int i;
  EMACS_INT max_nid;
  Lisp_Object node_id, key;

  max_nid = XINT (Fgethash (HASHKEY_MAX_NID, database, Qnil));

  XSETINT (node_id, 0);
  for (; CONSP (quarks); quarks = XCDR (quarks))
    {
      key = Fcons (node_id, XCAR (quarks));
      i = hash_lookup (h, key, &hash_code);
      if (i < 0)
	{
	  max_nid++;
	  XSETINT (node_id, max_nid);
	  hash_put (h, key, node_id, hash_code);
	}
      else
	node_id = HASH_VALUE (h, i);
    }
  Fputhash (node_id, value, database);

  Fputhash (HASHKEY_MAX_NID, make_number (max_nid), database);
  Fputhash (HASHKEY_QUERY_CACHE, Qnil, database);
}

/* Merge multiple resource entries specified by DATA into a resource
   database DATABASE.  DATA points to the head of a null-terminated
   string consisting of multiple resource lines.  It's like a
   combination of XrmGetStringDatabase and XrmMergeDatabases.  */

void
xrm_merge_string_database (database, data)
     XrmDatabase database;
     const char *data;
{
  Lisp_Object quarks_value;

  while (*data)
    {
      quarks_value = parse_resource_line (&data);
      if (!NILP (quarks_value))
	xrm_q_put_resource (database,
			    XCAR (quarks_value), XCDR (quarks_value));
    }
}

static Lisp_Object
xrm_q_get_resource_1 (database, node_id, quark_name, quark_class)
     XrmDatabase database;
     Lisp_Object node_id, quark_name, quark_class;
{
  struct Lisp_Hash_Table *h = XHASH_TABLE (database);
  Lisp_Object key, labels[3], value;
  int i, k;

  if (!CONSP (quark_name))
    return Fgethash (node_id, database, Qnil);

  /* First, try tight bindings */
  labels[0] = XCAR (quark_name);
  labels[1] = XCAR (quark_class);
  labels[2] = SINGLE_COMPONENT;

  key = Fcons (node_id, Qnil);
  for (k = 0; k < sizeof (labels) / sizeof (*labels); k++)
    {
      XSETCDR (key, labels[k]);
      i = hash_lookup (h, key, NULL);
      if (i >= 0)
	{
	  value = xrm_q_get_resource_1 (database, HASH_VALUE (h, i),
					XCDR (quark_name), XCDR (quark_class));
	  if (!NILP (value))
	    return value;
	}
    }

  /* Then, try loose bindings */
  XSETCDR (key, LOOSE_BINDING);
  i = hash_lookup (h, key, NULL);
  if (i >= 0)
    {
      value = xrm_q_get_resource_1 (database, HASH_VALUE (h, i),
				    quark_name, quark_class);
      if (!NILP (value))
	return value;
      else
	return xrm_q_get_resource_1 (database, node_id,
				     XCDR (quark_name), XCDR (quark_class));
    }
  else
    return Qnil;
}

static Lisp_Object
xrm_q_get_resource (database, quark_name, quark_class)
     XrmDatabase database;
     Lisp_Object quark_name, quark_class;
{
  return xrm_q_get_resource_1 (database, make_number (0),
			       quark_name, quark_class);
}

/* Retrieve a resource value for the specified NAME and CLASS from the
   resource database DATABASE.  It corresponds to XrmGetResource.  */

Lisp_Object
xrm_get_resource (database, name, class)
     XrmDatabase database;
     const char *name, *class;
{
  Lisp_Object key, query_cache, quark_name, quark_class, tmp;
  int i, nn, nc;
  struct Lisp_Hash_Table *h;
  unsigned hash_code;

  nn = strlen (name);
  nc = strlen (class);
  key = make_uninit_string (nn + nc + 1);
  strcpy (SDATA (key), name);
  strncpy (SDATA (key) + nn + 1, class, nc);

  query_cache = Fgethash (HASHKEY_QUERY_CACHE, database, Qnil);
  if (NILP (query_cache))
    {
      query_cache = make_hash_table (Qequal, make_number (DEFAULT_HASH_SIZE),
				     make_float (DEFAULT_REHASH_SIZE),
				     make_float (DEFAULT_REHASH_THRESHOLD),
				     Qnil, Qnil, Qnil);
      Fputhash (HASHKEY_QUERY_CACHE, query_cache, database);
    }
  h = XHASH_TABLE (query_cache);
  i = hash_lookup (h, key, &hash_code);
  if (i >= 0)
    return HASH_VALUE (h, i);

  quark_name = parse_resource_name (&name);
  if (*name != '\0')
    return Qnil;
  for (tmp = quark_name, nn = 0; CONSP (tmp); tmp = XCDR (tmp), nn++)
    if (!STRINGP (XCAR (tmp)))
      return Qnil;

  quark_class = parse_resource_name (&class);
  if (*class != '\0')
    return Qnil;
  for (tmp = quark_class, nc = 0; CONSP (tmp); tmp = XCDR (tmp), nc++)
    if (!STRINGP (XCAR (tmp)))
      return Qnil;

  if (nn != nc)
    return Qnil;
  else
    {
      tmp = xrm_q_get_resource (database, quark_name, quark_class);
      hash_put (h, key, tmp, hash_code);
      return tmp;
    }
}

static Lisp_Object
xrm_cfproperty_list_to_value (plist)
     CFPropertyListRef plist;
{
  CFTypeID type_id = CFGetTypeID (plist);

  if (type_id == CFStringGetTypeID ())
    return cfstring_to_lisp (plist);
  else if (type_id == CFNumberGetTypeID ())
    {
      CFStringRef string;
      Lisp_Object result = Qnil;

      string = CFStringCreateWithFormat (NULL, NULL, CFSTR ("%@"), plist);
      if (string)
	{
	  result = cfstring_to_lisp (string);
	  CFRelease (string);
	}
      return result;
    }
  else if (type_id == CFBooleanGetTypeID ())
    return build_string (CFBooleanGetValue (plist) ? "true" : "false");
  else if (type_id == CFDataGetTypeID ())
    return cfdata_to_lisp (plist);
  else
    return Qnil;
}

/* Create a new resource database from the preferences for the
   application APPLICATION.  APPLICATION is either a string that
   specifies an application ID, or NULL that represents the current
   application.  */

XrmDatabase
xrm_get_preference_database (application)
     const char *application;
{
  CFStringRef app_id, *keys, user_doms[2], host_doms[2];
  CFMutableSetRef key_set = NULL;
  CFArrayRef key_array;
  CFIndex index, count;
  char *res_name;
  XrmDatabase database;
  Lisp_Object quarks = Qnil, value = Qnil;
  CFPropertyListRef plist;
  int iu, ih;
  struct gcpro gcpro1, gcpro2, gcpro3;

  user_doms[0] = kCFPreferencesCurrentUser;
  user_doms[1] = kCFPreferencesAnyUser;
  host_doms[0] = kCFPreferencesCurrentHost;
  host_doms[1] = kCFPreferencesAnyHost;

  database = xrm_create_database ();

  GCPRO3 (database, quarks, value);

  app_id = kCFPreferencesCurrentApplication;
  if (application)
    {
      app_id = cfstring_create_with_utf8_cstring (application);
      if (app_id == NULL)
	goto out;
    }
  if (!CFPreferencesAppSynchronize (app_id))
    goto out;

  key_set = CFSetCreateMutable (NULL, 0, &kCFCopyStringSetCallBacks);
  if (key_set == NULL)
    goto out;
  for (iu = 0; iu < sizeof (user_doms) / sizeof (*user_doms) ; iu++)
    for (ih = 0; ih < sizeof (host_doms) / sizeof (*host_doms); ih++)
      {
	key_array = CFPreferencesCopyKeyList (app_id, user_doms[iu],
					      host_doms[ih]);
	if (key_array)
	  {
	    count = CFArrayGetCount (key_array);
	    for (index = 0; index < count; index++)
	      CFSetAddValue (key_set,
			     CFArrayGetValueAtIndex (key_array, index));
	    CFRelease (key_array);
	  }
      }

  count = CFSetGetCount (key_set);
  keys = xmalloc (sizeof (CFStringRef) * count);
  CFSetGetValues (key_set, (const void **)keys);
  for (index = 0; index < count; index++)
    {
      res_name = SDATA (cfstring_to_lisp_nodecode (keys[index]));
      quarks = parse_resource_name (&res_name);
      if (!(NILP (quarks) || *res_name))
	{
	  plist = CFPreferencesCopyAppValue (keys[index], app_id);
	  value = xrm_cfproperty_list_to_value (plist);
	  CFRelease (plist);
	  if (!NILP (value))
	    xrm_q_put_resource (database, quarks, value);
	}
    }

  xfree (keys);
 out:
  if (key_set)
    CFRelease (key_set);
  CFRelease (app_id);

  UNGCPRO;

  return database;
}


Lisp_Object Qmac_file_alias_p;

void
initialize_applescript ()
{
  AEDesc null_desc;
  OSAError osaerror;

  /* if open fails, as_scripting_component is set to NULL.  Its
     subsequent use in OSA calls will fail with badComponentInstance
     error.  */
  as_scripting_component = OpenDefaultComponent (kOSAComponentType,
						 kAppleScriptSubtype);

  null_desc.descriptorType = typeNull;
  null_desc.dataHandle = 0;
  osaerror = OSAMakeContext (as_scripting_component, &null_desc,
			     kOSANullScript, &as_script_context);
  if (osaerror)
    as_script_context = kOSANullScript;
      /* use default context if create fails */
}


void
terminate_applescript()
{
  OSADispose (as_scripting_component, as_script_context);
  CloseComponent (as_scripting_component);
}

/* Convert a lisp string to the 4 byte character code.  */

OSType
mac_get_code_from_arg(Lisp_Object arg, OSType defCode)
{
  OSType result;
  if (NILP(arg))
    {
      result = defCode;
    }
  else
    {
      /* check type string */
      CHECK_STRING(arg);
      if (SBYTES (arg) != 4)
	{
	  error ("Wrong argument: need string of length 4 for code");
	}
      result = EndianU32_BtoN (*((UInt32 *) SDATA (arg)));
    }
  return result;
}

/* Convert the 4 byte character code into a 4 byte string.  */

Lisp_Object
mac_get_object_from_code(OSType defCode)
{
  UInt32 code = EndianU32_NtoB (defCode);

  return make_unibyte_string ((char *)&code, 4);
}


DEFUN ("mac-get-file-creator", Fmac_get_file_creator, Smac_get_file_creator, 1, 1, 0,
       doc: /* Get the creator code of FILENAME as a four character string. */)
     (filename)
     Lisp_Object filename;
{
  OSStatus status;
  FSRef fref;
  Lisp_Object result = Qnil;
  CHECK_STRING (filename);

  if (NILP(Ffile_exists_p(filename)) || !NILP(Ffile_directory_p(filename))) {
    return Qnil;
  }
  filename = Fexpand_file_name (filename, Qnil);

  BLOCK_INPUT;
  status = FSPathMakeRef(SDATA(ENCODE_FILE(filename)), &fref, NULL);

  if (status == noErr)
    {
      FSCatalogInfo catalogInfo;

      status = FSGetCatalogInfo(&fref, kFSCatInfoFinderInfo,
				&catalogInfo, NULL, NULL, NULL);
      if (status == noErr)
	{
	  result = mac_get_object_from_code(((FileInfo*)&catalogInfo.finderInfo)->fileCreator);
	}
    }
  UNBLOCK_INPUT;
  if (status != noErr) {
    error ("Error while getting file information.");
  }
  return result;
}

DEFUN ("mac-get-file-type", Fmac_get_file_type, Smac_get_file_type, 1, 1, 0,
       doc: /* Get the type code of FILENAME as a four character string. */)
     (filename)
     Lisp_Object filename;
{
  OSStatus status;
  FSRef fref;
  Lisp_Object result = Qnil;
  CHECK_STRING (filename);

  if (NILP(Ffile_exists_p(filename)) || !NILP(Ffile_directory_p(filename))) {
    return Qnil;
  }
  filename = Fexpand_file_name (filename, Qnil);

  BLOCK_INPUT;
  status = FSPathMakeRef(SDATA(ENCODE_FILE(filename)), &fref, NULL);

  if (status == noErr)
    {
      FSCatalogInfo catalogInfo;

      status = FSGetCatalogInfo(&fref, kFSCatInfoFinderInfo,
				&catalogInfo, NULL, NULL, NULL);
      if (status == noErr)
	{
	  result = mac_get_object_from_code(((FileInfo*)&catalogInfo.finderInfo)->fileType);
	}
    }
  UNBLOCK_INPUT;
  if (status != noErr) {
    error ("Error while getting file information.");
  }
  return result;
}

DEFUN ("mac-set-file-creator", Fmac_set_file_creator, Smac_set_file_creator, 1, 2, 0,
       doc: /* Set creator code of file FILENAME to CODE.
If non-nil, CODE must be a 4-character string.  Otherwise, 'EMAx' is
assumed. Return non-nil if successful.  */)
     (filename, code)
     Lisp_Object filename, code;
{
  OSStatus status;
  FSRef fref;
  OSType cCode;
  CHECK_STRING (filename);

  cCode = mac_get_code_from_arg(code, MAC_EMACS_CREATOR_CODE);

  if (NILP(Ffile_exists_p(filename)) || !NILP(Ffile_directory_p(filename))) {
    return Qnil;
  }
  filename = Fexpand_file_name (filename, Qnil);

  BLOCK_INPUT;
  status = FSPathMakeRef(SDATA(ENCODE_FILE(filename)), &fref, NULL);

  if (status == noErr)
    {
      FSCatalogInfo catalogInfo;
      FSRef parentDir;
      status = FSGetCatalogInfo(&fref, kFSCatInfoFinderInfo,
				&catalogInfo, NULL, NULL, &parentDir);
      if (status == noErr)
	{
	((FileInfo*)&catalogInfo.finderInfo)->fileCreator = cCode;
	status = FSSetCatalogInfo(&fref, kFSCatInfoFinderInfo, &catalogInfo);
	/* TODO: on Mac OS 10.2, we need to touch the parent dir, FNNotify? */
	}
    }
  UNBLOCK_INPUT;
  if (status != noErr) {
    error ("Error while setting creator information.");
  }
  return Qt;
}

DEFUN ("mac-set-file-type", Fmac_set_file_type, Smac_set_file_type, 2, 2, 0,
       doc: /* Set file code of file FILENAME to CODE.
CODE must be a 4-character string.  Return non-nil if successful.  */)
     (filename, code)
     Lisp_Object filename, code;
{
  OSStatus status;
  FSRef fref;
  OSType cCode;
  CHECK_STRING (filename);

  cCode = mac_get_code_from_arg(code, 0); /* Default to empty code*/

  if (NILP(Ffile_exists_p(filename)) || !NILP(Ffile_directory_p(filename))) {
    return Qnil;
  }
  filename = Fexpand_file_name (filename, Qnil);

  BLOCK_INPUT;
  status = FSPathMakeRef(SDATA(ENCODE_FILE(filename)), &fref, NULL);

  if (status == noErr)
    {
      FSCatalogInfo catalogInfo;
      FSRef parentDir;
      status = FSGetCatalogInfo(&fref, kFSCatInfoFinderInfo,
				&catalogInfo, NULL, NULL, &parentDir);
      if (status == noErr)
	{
	((FileInfo*)&catalogInfo.finderInfo)->fileType = cCode;
	status = FSSetCatalogInfo(&fref, kFSCatInfoFinderInfo, &catalogInfo);
	/* TODO: on Mac OS 10.2, we need to touch the parent dir, FNNotify? */
	}
    }
  UNBLOCK_INPUT;
  if (status != noErr) {
    error ("Error while setting creator information.");
  }
  return Qt;
}

DEFUN ("mac-file-alias-p", Fmac_file_alias_p, Smac_file_alias_p, 1, 1, 0,
       doc: /* Return non-nil if file FILENAME is the name of an alias file.
The value is the file referred to by the alias file, as a string.
Otherwise it returns nil.

This function returns t when given the name of an alias file
containing an unresolvable alias.  */)
     (filename)
     Lisp_Object filename;
{
  OSStatus err;
  Lisp_Object handler, result = Qnil;
  FSRef fref;

  CHECK_STRING (filename);
  filename = Fexpand_file_name (filename, Qnil);

  /* If the file name has special constructs in it,
     call the corresponding file handler.  */
  handler = Ffind_file_name_handler (filename, Qmac_file_alias_p);
  if (!NILP (handler))
    return call2 (handler, Qmac_file_alias_p, filename);

  BLOCK_INPUT;
  err = FSPathMakeRef (SDATA (ENCODE_FILE (filename)), &fref, NULL);
  if (err == noErr)
    {
      Boolean alias_p = false, folder_p;

      err = FSResolveAliasFileWithMountFlags (&fref, false,
					      &folder_p, &alias_p,
					      kResolveAliasFileNoUI);
      if (err != noErr)
	result = Qt;
      else if (alias_p)
	{
	  char buf[MAXPATHLEN];

	  err = FSRefMakePath (&fref, buf, sizeof (buf));
	  if (err == noErr)
	    {
	      result = make_unibyte_string (buf, strlen (buf));
	      if (buf[0] == '/' && index (buf, ':'))
		result = concat2 (build_string ("/:"), result);
	      result = DECODE_FILE (result);
	    }
	}
    }
  UNBLOCK_INPUT;

  return result;
}

/* Moving files to the system recycle bin.
   Used by `move-file-to-trash' instead of the default moving to ~/.Trash  */
DEFUN ("system-move-file-to-trash", Fsystem_move_file_to_trash,
       Ssystem_move_file_to_trash, 1, 1, 0,
       doc: /* Move file or directory named FILENAME to the recycle bin.  */)
     (filename)
     Lisp_Object filename;
{
  OSStatus err;
  FSRef fref;
  Lisp_Object errstring = Qnil;
  Lisp_Object handler;
  Lisp_Object encoded_file;
  Lisp_Object operation;

  operation = Qdelete_file;
  if (!NILP (Ffile_directory_p (filename))
      && NILP (Ffile_symlink_p (filename)))
    {
      operation = intern ("delete-directory");
      filename = Fdirectory_file_name (filename);
    }
  filename = Fexpand_file_name (filename, Qnil);

  handler = Ffind_file_name_handler (filename, operation);
  if (!NILP (handler))
    return call2 (handler, operation, filename);

  encoded_file = ENCODE_FILE (filename);

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1040
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1040 && MAC_OS_X_VERSION_MIN_REQUIRED >= 1020
  if (FSPathMakeRefWithOptions != NULL)
#endif
    {
      BLOCK_INPUT;
      err = FSPathMakeRefWithOptions (SDATA (encoded_file),
				      kFSPathMakeRefDoNotFollowLeafSymlink,
				      &fref, NULL);
      UNBLOCK_INPUT;
    }
#endif
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1040 && MAC_OS_X_VERSION_MIN_REQUIRED >= 1020
  else				/* FSPathMakeRefWithOptions == NULL */
#endif
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1040 || (MAC_OS_X_VERSION_MIN_REQUIRED < 1040 && MAC_OS_X_VERSION_MIN_REQUIRED >= 1020)
    {
      struct stat st;

      if (lstat (SDATA (encoded_file), &st) < 0)
	report_file_error ("Removing old name", list1 (filename));

      BLOCK_INPUT;
      if (!S_ISLNK (st.st_mode))
	err = FSPathMakeRef (SDATA (encoded_file), &fref, NULL);
      else
	{
	  char *leaf = rindex (SDATA (encoded_file), '/') + 1;
	  size_t parent_len = leaf - (char *) SDATA (encoded_file);
	  char *parent = alloca (parent_len + 1);
	  FSRef parent_ref;

	  memcpy (parent, SDATA (encoded_file), parent_len);
	  parent[parent_len] = '\0';
	  err = FSPathMakeRef (parent, &parent_ref, NULL);
	  if (err == noErr)
	    {
	      CFStringRef name_str =
		CFStringCreateWithBytes (NULL, leaf,
					 SBYTES (encoded_file) - parent_len,
					 kCFStringEncodingUTF8, false);

	      if (name_str)
		{
		  UniCharCount name_len = CFStringGetLength (name_str);
		  UniChar *name = alloca (sizeof (UniChar) * name_len);

		  CFStringGetCharacters (name_str, CFRangeMake (0, name_len),
					 name);
		  err = FSMakeFSRefUnicode (&parent_ref, name_len, name,
					    kTextEncodingUnknown, &fref);
		  CFRelease (name_str);
		}
	      else
		err = memFullErr;
	    }
	}
      UNBLOCK_INPUT;
    }
#endif

  if (err == noErr)
    {
      BLOCK_INPUT;
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1050
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1050 && MAC_OS_X_VERSION_MIN_REQUIRED >= 1020
      if (FSMoveObjectToTrashSync != NULL)
#endif
	{
	  /* FSPathMoveObjectToTrashSync tries to delete the
	     destination of the specified symbolic link.  So we use
	     FSMoveObjectToTrashSync for an FSRef created with
	     kFSPathMakeRefDoNotFollowLeafSymlink.  */
	  err = FSMoveObjectToTrashSync (&fref, NULL,
					 kFSFileOperationDefaultOptions);
	}
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1050 && MAC_OS_X_VERSION_MIN_REQUIRED >= 1020
      else				/* FSMoveObjectToTrashSync == NULL */
#endif
#endif
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1050 || (MAC_OS_X_VERSION_MIN_REQUIRED < 1050 && MAC_OS_X_VERSION_MIN_REQUIRED >= 1020)
	{
	  const OSType finderSignature = 'MACS';
	  UInt32 response;
	  AEDesc desc;
	  AppleEvent event, reply;

	  err = Gestalt (gestaltSystemVersion, &response);
	  if (err == noErr)
	    {
	      if (response < 0x1030)
		/* Coerce to typeAlias as Finder on Mac OS X 10.2
		   doesn't accept FSRef.  We should not do this on
		   later versions because it leads to the deletion of
		   the destination of the symbolic link.  */
		err = AECoercePtr (typeFSRef, &fref, sizeof (FSRef),
				   typeAlias, &desc);
	      else
		/* Specifying typeFileURL as target type enables us to
		   delete the specified symbolic link itself on Mac OS
		   X 10.4 and later.  But that doesn't work on Mac OS
		   X 10.3.  That's why we created an FSRef without
		   following the link at the leaf position.  */
		err = AECreateDesc (typeFSRef, &fref, sizeof (FSRef), &desc);
	    }
	  if (err == noErr)
	    {
	      err = AEBuildAppleEvent (kAECoreSuite, kAEDelete,
				       typeApplSignature,
				       &finderSignature, sizeof (OSType),
				       kAutoGenerateReturnID, kAnyTransactionID,
				       &event, NULL, "'----':@", &desc);
	      AEDisposeDesc (&desc);
	    }
	  if (err == noErr)
	    {
	      err = AESendMessage (&event, &reply,
				   kAEWaitReply | kAENeverInteract,
				   kAEDefaultTimeout);
	      AEDisposeDesc (&event);
	    }
	  if (err == noErr)
	    {
	      if (reply.descriptorType != typeNull)
		{
		  OSStatus err1, handler_err;
		  AEDesc desc;

		  err1 = AEGetParamPtr (&reply, keyErrorNumber, typeSInt32,
					NULL, &handler_err,
					sizeof (OSStatus), NULL);
		  if (err1 != errAEDescNotFound)
		    err = handler_err;
		  err1 = AEGetParamDesc (&reply, keyErrorString,
					 typeUTF8Text, /* Needs 10.2 */
					 &desc);
		  if (err1 == noErr)
		    {
		      errstring =
			make_uninit_string (AEGetDescDataSize (&desc));
		      err1 = AEGetDescData (&desc, SDATA (errstring),
					   SBYTES (errstring));
		      if (err1 == noErr)
			errstring =
			  code_convert_string_norecord (errstring, Qutf_8, 0);
		      else
			errstring = Qnil;
		      AEDisposeDesc (&desc);
		    }
		}
	      AEDisposeDesc (&reply);
	    }
	}
#endif
      UNBLOCK_INPUT;
    }

  if (err != noErr)
    {
      errno = 0;
      if (NILP (errstring))
	{
	  switch (err)
	    {
	    case fnfErr:
	      errno = ENOENT;
	      break;

	    case afpAccessDenied:
	      errno = EACCES;
	      break;

	    default:
	      errstring = concat2 (build_string ("Mac error "),
				   Fnumber_to_string (make_number (err)));
	      break;
	    }
	}
      if (errno)
	report_file_error ("Removing old name", list1 (filename));
      else
	xsignal (Qfile_error, list3 (build_string ("Removing old name"),
				     errstring, filename));
    }

  return Qnil;
}


/* Compile and execute the AppleScript SCRIPT and return the error
   status as function value.  A zero is returned if compilation and
   execution is successful, in which case *RESULT is set to a Lisp
   string containing the resulting script value.  Otherwise, the Mac
   error code is returned and *RESULT is set to an error Lisp string.
   For documentation on the MacOS scripting architecture, see Inside
   Macintosh - Interapplication Communications: Scripting
   Components.  */

long
do_applescript (script, result)
     Lisp_Object script, *result;
{
  AEDesc script_desc, result_desc, error_desc, *desc = NULL;
  OSErr error;
  OSAError osaerror;
  DescType desc_type;

  *result = Qnil;

  if (!as_scripting_component)
    initialize_applescript();

  if (STRING_MULTIBYTE (script))
    {
      desc_type = typeUnicodeText;
      script = code_convert_string_norecord (script,
#ifdef WORDS_BIG_ENDIAN
					     intern ("utf-16be"),
#else
					     intern ("utf-16le"),
#endif
					     1);
    }
  else
    desc_type = typeChar;

  error = AECreateDesc (desc_type, SDATA (script), SBYTES (script),
			&script_desc);
  if (error)
    return error;

  osaerror = OSADoScript (as_scripting_component, &script_desc, kOSANullScript,
			  desc_type, kOSAModeNull, &result_desc);

  if (osaerror == noErr)
    /* success: retrieve resulting script value */
    desc = &result_desc;
  else if (osaerror == errOSAScriptError)
    /* error executing AppleScript: retrieve error message */
    if (!OSAScriptError (as_scripting_component, kOSAErrorMessage, desc_type,
			 &error_desc))
      desc = &error_desc;

  if (desc)
    {
      *result = make_uninit_string (AEGetDescDataSize (desc));
      AEGetDescData (desc, SDATA (*result), SBYTES (*result));
      if (desc_type == typeUnicodeText)
	*result = code_convert_string_norecord (*result,
#ifdef WORDS_BIG_ENDIAN
						intern ("utf-16be"),
#else
						intern ("utf-16le"),
#endif
						0);
      AEDisposeDesc (desc);
    }

  AEDisposeDesc (&script_desc);

  return osaerror;
}


DEFUN ("do-applescript", Fdo_applescript, Sdo_applescript, 1, 1, 0,
       doc: /* Compile and execute AppleScript SCRIPT and return the result.
If compilation and execution are successful, the resulting script
value is returned as a string.  Otherwise the function aborts and
displays the error message returned by the AppleScript scripting
component.

If SCRIPT is a multibyte string, it is regarded as a Unicode text.
Otherwise, SCRIPT is regarded as a byte sequence in a Mac traditional
encoding specified by `mac-system-script-code', just as in Emacs 22.
Note that a unibyte ASCII-only SCRIPT does not always have the same
meaning as the multibyte counterpart.  For example, `\\x5c' in a
unibyte SCRIPT is interpreted as a yen sign when the value of
`mac-system-script-code' is 1 (smJapanese), but the same character in
a multibyte SCRIPT is interpreted as a reverse solidus.  You may want
to apply `string-to-multibyte' to the script if it is given as an
ASCII-only string literal.  */)
    (script)
    Lisp_Object script;
{
  Lisp_Object result;
  long status;

  CHECK_STRING (script);

  BLOCK_INPUT;
  {
    extern long mac_appkit_do_applescript P_ ((Lisp_Object, Lisp_Object *));

    if (!inhibit_window_system)
      status = mac_appkit_do_applescript (script, &result);
    else
      status = do_applescript (script, &result);
  }
  UNBLOCK_INPUT;
  if (status == 0)
    return result;
  else if (!STRINGP (result))
    error ("AppleScript error %d", status);
  else
    error ("%s", SDATA (result));
}


DEFUN ("mac-coerce-ae-data", Fmac_coerce_ae_data, Smac_coerce_ae_data, 3, 3, 0,
       doc: /* Coerce Apple event data SRC-DATA of type SRC-TYPE to DST-TYPE.
Each type should be a string of length 4 or the symbol
`undecoded-file-name'.  */)
  (src_type, src_data, dst_type)
     Lisp_Object src_type, src_data, dst_type;
{
  OSErr err;
  Lisp_Object result = Qnil;
  DescType src_desc_type, dst_desc_type;
  AEDesc dst_desc;

  CHECK_STRING (src_data);
  if (EQ (src_type, Qundecoded_file_name))
    src_desc_type = TYPE_FILE_NAME;
  else
    src_desc_type = mac_get_code_from_arg (src_type, 0);

  if (EQ (dst_type, Qundecoded_file_name))
    dst_desc_type = TYPE_FILE_NAME;
  else
    dst_desc_type = mac_get_code_from_arg (dst_type, 0);

  BLOCK_INPUT;
  err = AECoercePtr (src_desc_type, SDATA (src_data), SBYTES (src_data),
		     dst_desc_type, &dst_desc);
  if (err == noErr)
    {
      result = Fcdr (mac_aedesc_to_lisp (&dst_desc));
      AEDisposeDesc (&dst_desc);
    }
  UNBLOCK_INPUT;

  return result;
}


static Lisp_Object Qxml, Qxml1, Qbinary1, QCmime_charset;
static Lisp_Object QNFD, QNFKD, QNFC, QNFKC, QHFS_plus_D, QHFS_plus_C;

DEFUN ("mac-get-preference", Fmac_get_preference, Smac_get_preference, 1, 4, 0,
       doc: /* Return the application preference value for KEY.
KEY is either a string specifying a preference key, or a list of key
strings.  If it is a list, the (i+1)-th element is used as a key for
the CFDictionary value obtained by the i-th element.  Return nil if
lookup is failed at some stage.

Optional arg APPLICATION is an application ID string.  If omitted or
nil, that stands for the current application.

Optional args FORMAT and HASH-BOUND specify the data format of the
return value (see `mac-convert-property-list').  FORMAT also accepts
`xml' as a synonym of `xml1' for compatibility.  */)
     (key, application, format, hash_bound)
     Lisp_Object key, application, format, hash_bound;
{
  CFStringRef app_id, key_str;
  CFPropertyListRef app_plist = NULL, plist;
  Lisp_Object result = Qnil, tmp;
  struct gcpro gcpro1, gcpro2;

  if (STRINGP (key))
    key = Fcons (key, Qnil);
  else
    {
      CHECK_CONS (key);
      for (tmp = key; CONSP (tmp); tmp = XCDR (tmp))
	{
	  CHECK_STRING_CAR (tmp);
	  QUIT;
	}
      CHECK_LIST_END (tmp, key);
    }
  if (!NILP (application))
    CHECK_STRING (application);
  CHECK_SYMBOL (format);
  if (!NILP (hash_bound))
    CHECK_NUMBER (hash_bound);

  GCPRO2 (key, format);

  BLOCK_INPUT;

  app_id = kCFPreferencesCurrentApplication;
  if (!NILP (application))
    {
      app_id = cfstring_create_with_string (application);
      if (app_id == NULL)
	goto out;
    }
  if (!CFPreferencesAppSynchronize (app_id))
    goto out;

  key_str = cfstring_create_with_string (XCAR (key));
  if (key_str == NULL)
    goto out;
  app_plist = CFPreferencesCopyAppValue (key_str, app_id);
  CFRelease (key_str);
  if (app_plist == NULL)
    goto out;

  plist = app_plist;
  for (key = XCDR (key); CONSP (key); key = XCDR (key))
    {
      if (CFGetTypeID (plist) != CFDictionaryGetTypeID ())
	break;
      key_str = cfstring_create_with_string (XCAR (key));
      if (key_str == NULL)
	goto out;
      plist = CFDictionaryGetValue (plist, key_str);
      CFRelease (key_str);
      if (plist == NULL)
	goto out;
    }

  if (NILP (key))
    {
      if (EQ (format, Qxml) || EQ (format, Qxml1))
	result = cfproperty_list_to_string (plist,
					    kCFPropertyListXMLFormat_v1_0);
      else if (EQ (format, Qbinary1))
	result = cfproperty_list_to_string (plist,
					    kCFPropertyListBinaryFormat_v1_0);
      else
	result =
	  cfproperty_list_to_lisp (plist, EQ (format, Qt),
				   NILP (hash_bound) ? -1 : XINT (hash_bound));
    }

 out:
  if (app_plist)
    CFRelease (app_plist);
  CFRelease (app_id);

  UNBLOCK_INPUT;

  UNGCPRO;

  return result;
}

DEFUN ("mac-convert-property-list", Fmac_convert_property_list, Smac_convert_property_list, 1, 3, 0,
       doc: /* Convert Core Foundation PROPERTY-LIST to FORMAT.
PROPERTY-LIST should be either a string whose data is in some Core
Foundation property list file format (e.g., XML or binary version 1),
or a Lisp representation of a property list with type tags.  Return
nil if PROPERTY-LIST is ill-formatted.

In the Lisp representation of a property list, each Core Foundation
object is converted into a corresponding Lisp object as follows:

  Core Foundation    Lisp                           Tag
  ------------------------------------------------------------
  CFString           Multibyte string               string
  CFNumber           Integer, float, or string      number
  CFBoolean          Symbol (t or nil)              boolean
  CFDate             List of three integers         date
                       (cf. `current-time')
  CFData             Unibyte string                 data
  CFArray            Vector                         array
  CFDictionary       Alist or hash table            dictionary
                       (depending on HASH-BOUND)

If the representation has type tags, each object is a cons of the tag
symbol in the `Tag' row and a value of the type in the `Lisp' row.

Optional arg FORMAT specifies the data format of the return value.  If
omitted or nil, a Lisp representation without tags is returned.  If
FORMAT is t, a Lisp representation with tags is returned.  If FORMAT
is `xml1' or `binary1', a unibyte string is returned as an XML or
binary representation version 1, respectively.

Optional arg HASH-BOUND specifies which kinds of the Lisp objects,
alists or hash tables, are used as the targets of the conversion from
CFDictionary.  If HASH-BOUND is a negative integer or nil, always
generate alists.  If HASH-BOUND >= 0, generate an alist if the number
of keys in the dictionary is smaller than HASH-BOUND, and a hash table
otherwise.  */)
     (property_list, format, hash_bound)
     Lisp_Object property_list, format, hash_bound;
{
  Lisp_Object result = Qnil;
  CFPropertyListRef plist;
  struct gcpro gcpro1, gcpro2;

  if (!CONSP (property_list))
    CHECK_STRING (property_list);
  if (!NILP (hash_bound))
    CHECK_NUMBER (hash_bound);

  GCPRO2 (property_list, format);

  BLOCK_INPUT;

  if (CONSP (property_list))
    plist = cfproperty_list_create_with_lisp (property_list);
  else
    plist = cfproperty_list_create_with_string (property_list);
  if (plist)
    {
      if (EQ (format, Qxml1))
	result = cfproperty_list_to_string (plist,
					    kCFPropertyListXMLFormat_v1_0);
      else if (EQ (format, Qbinary1))
	result = cfproperty_list_to_string (plist,
					    kCFPropertyListBinaryFormat_v1_0);
      else
	result =
	  cfproperty_list_to_lisp (plist, EQ (format, Qt),
				   NILP (hash_bound) ? -1 : XINT (hash_bound));
      CFRelease (plist);
    }

  UNBLOCK_INPUT;

  UNGCPRO;

  return result;
}

static CFStringEncoding
get_cfstring_encoding_from_lisp (obj)
     Lisp_Object obj;
{
  CFStringRef iana_name;
  CFStringEncoding encoding = kCFStringEncodingInvalidId;

  if (NILP (obj))
    return kCFStringEncodingUnicode;

  if (INTEGERP (obj))
    return XINT (obj);

  if (SYMBOLP (obj) && !NILP (Fcoding_system_p (obj)))
    {
      Lisp_Object attrs, plist;

      attrs = AREF (CODING_SYSTEM_SPEC (obj), 0);
      plist = CODING_ATTR_PLIST (attrs);
      obj = Fplist_get (plist, QCmime_charset);
    }

  if (SYMBOLP (obj))
    obj = SYMBOL_NAME (obj);

  if (STRINGP (obj))
    {
      iana_name = cfstring_create_with_string (obj);
      if (iana_name)
	{
	  encoding = CFStringConvertIANACharSetNameToEncoding (iana_name);
	  CFRelease (iana_name);
	}
    }

  return encoding;
}

static CFStringRef
cfstring_create_normalized (str, symbol)
     CFStringRef str;
     Lisp_Object symbol;
{
  int form = -1;
  TextEncodingVariant variant;
  float initial_mag = 0.0;
  CFStringRef result = NULL;

  if (EQ (symbol, QNFD))
    form = kCFStringNormalizationFormD;
  else if (EQ (symbol, QNFKD))
    form = kCFStringNormalizationFormKD;
  else if (EQ (symbol, QNFC))
    form = kCFStringNormalizationFormC;
  else if (EQ (symbol, QNFKC))
    form = kCFStringNormalizationFormKC;
  else if (EQ (symbol, QHFS_plus_D))
    {
      variant = kUnicodeHFSPlusDecompVariant;
      initial_mag = 1.5;
    }
  else if (EQ (symbol, QHFS_plus_C))
    {
      variant = kUnicodeHFSPlusCompVariant;
      initial_mag = 1.0;
    }

  if (form >= 0)
    {
      CFMutableStringRef mut_str = CFStringCreateMutableCopy (NULL, 0, str);

      if (mut_str)
	{
	  CFStringNormalize (mut_str, form);
	  result = mut_str;
	}
    }
  else if (initial_mag > 0.0)
    {
      UnicodeToTextInfo uni = NULL;
      UnicodeMapping map;
      CFIndex length;
      UniChar *in_text, *buffer = NULL, *out_buf = NULL;
      OSStatus err = noErr;
      ByteCount out_read, out_size, out_len;

      map.unicodeEncoding = CreateTextEncoding (kTextEncodingUnicodeDefault,
						kUnicodeNoSubset,
						kTextEncodingDefaultFormat);
      map.otherEncoding = CreateTextEncoding (kTextEncodingUnicodeDefault,
					      variant,
					      kTextEncodingDefaultFormat);
      map.mappingVersion = kUnicodeUseLatestMapping;

      length = CFStringGetLength (str);
      out_size = (int)((float)length * initial_mag) * sizeof (UniChar);
      if (out_size < 32)
	out_size = 32;

      in_text = (UniChar *)CFStringGetCharactersPtr (str);
      if (in_text == NULL)
	{
	  buffer = xmalloc (sizeof (UniChar) * length);
	  CFStringGetCharacters (str, CFRangeMake (0, length), buffer);
	  in_text = buffer;
	}

      if (in_text)
	err = CreateUnicodeToTextInfo (&map, &uni);
      while (err == noErr)
	{
	  out_buf = xmalloc (out_size);
	  err = ConvertFromUnicodeToText (uni, length * sizeof (UniChar),
					  in_text,
					  kUnicodeDefaultDirectionMask,
					  0, NULL, NULL, NULL,
					  out_size, &out_read, &out_len,
					  out_buf);
	  if (err == noErr && out_read < length * sizeof (UniChar))
	    {
	      xfree (out_buf);
	      out_size += length;
	    }
	  else
	    break;
	}
      if (err == noErr)
	result = CFStringCreateWithCharacters (NULL, out_buf,
					       out_len / sizeof (UniChar));
      if (uni)
	DisposeUnicodeToTextInfo (&uni);
      xfree (out_buf);
      xfree (buffer);
    }
  else
    {
      result = str;
      CFRetain (result);
    }

  return result;
}

DEFUN ("mac-code-convert-string", Fmac_code_convert_string, Smac_code_convert_string, 3, 4, 0,
       doc: /* Convert STRING from SOURCE encoding to TARGET encoding.
The conversion is performed using the converter provided by the system.
Each encoding is specified by either a coding system symbol, a mime
charset string, or an integer as a CFStringEncoding value.  An encoding
of nil means UTF-16 in native byte order, no byte order mark.
On Mac OS X 10.2 and later, you can do Unicode Normalization by
specifying the optional argument NORMALIZATION-FORM with a symbol NFD,
NFKD, NFC, NFKC, HFS+D, or HFS+C.
On successful conversion, return the result string, else return nil.  */)
     (string, source, target, normalization_form)
     Lisp_Object string, source, target, normalization_form;
{
  Lisp_Object result = Qnil;
  struct gcpro gcpro1, gcpro2, gcpro3, gcpro4;
  CFStringEncoding src_encoding, tgt_encoding;
  CFStringRef str = NULL;

  CHECK_STRING (string);
  if (!INTEGERP (source) && !STRINGP (source))
    CHECK_SYMBOL (source);
  if (!INTEGERP (target) && !STRINGP (target))
    CHECK_SYMBOL (target);
  CHECK_SYMBOL (normalization_form);

  GCPRO4 (string, source, target, normalization_form);

  BLOCK_INPUT;

  src_encoding = get_cfstring_encoding_from_lisp (source);
  tgt_encoding = get_cfstring_encoding_from_lisp (target);

  /* We really want string_to_unibyte, but since it doesn't exist yet, we
     use string_as_unibyte which works as well, except for the fact that
     it's too permissive (it doesn't check that the multibyte string only
     contain single-byte chars).  */
  string = Fstring_as_unibyte (string);
  if (src_encoding != kCFStringEncodingInvalidId
      && tgt_encoding != kCFStringEncodingInvalidId)
    str = CFStringCreateWithBytes (NULL, SDATA (string), SBYTES (string),
				   src_encoding, !NILP (source));
  if (str)
    {
      CFStringRef saved_str = str;

      str = cfstring_create_normalized (saved_str, normalization_form);
      CFRelease (saved_str);
    }
  if (str)
    {
      CFIndex str_len, buf_len;

      str_len = CFStringGetLength (str);
      if (CFStringGetBytes (str, CFRangeMake (0, str_len), tgt_encoding, 0,
			    !NILP (target), NULL, 0, &buf_len) == str_len)
	{
	  result = make_uninit_string (buf_len);
	  CFStringGetBytes (str, CFRangeMake (0, str_len), tgt_encoding, 0,
			    !NILP (target), SDATA (result), buf_len, NULL);
	}
      CFRelease (str);
    }

  UNBLOCK_INPUT;

  UNGCPRO;

  return result;
}

DEFUN ("mac-process-hi-command", Fmac_process_hi_command, Smac_process_hi_command, 1, 1, 0,
       doc: /* Send a HI command whose ID is COMMAND-ID to the command chain.
COMMAND-ID must be a 4-character string.  Some common command IDs are
defined in the Carbon Event Manager.  */)
     (command_id)
     Lisp_Object command_id;
{
  OSStatus err;
  HICommand command;

  bzero (&command, sizeof (HICommand));
  command.commandID = mac_get_code_from_arg (command_id, 0);

  BLOCK_INPUT;
  err = ProcessHICommand (&command);
  UNBLOCK_INPUT;

  if (err != noErr)
    error ("HI command (command ID: '%s') not handled.", SDATA (command_id));

  return Qnil;
}

static ScriptCode
mac_get_system_script_code ()
{
  ScriptCode result;
  OSStatus err;

  err = RevertTextEncodingToScriptInfo (CFStringGetSystemEncoding (),
					&result, NULL, NULL);
  if (err != noErr)
    result = 0;

  return result;
}

static Lisp_Object
mac_get_system_locale ()
{
#if !__LP64__
  OSStatus err;
  LangCode lang;
  RegionCode region;
  LocaleRef locale;
  Str255 str;

  lang = GetScriptVariable (smSystemScript, smScriptLang);
  region = GetScriptManagerVariable (smRegionCode);
  err = LocaleRefFromLangOrRegionCode (lang, region, &locale);
  if (err == noErr)
    err = LocaleRefGetPartString (locale, kLocaleAllPartsMask,
				  sizeof (str), str);
  if (err == noErr)
    return build_string (str);
  else
    return Qnil;
#else
  return Qnil;
#endif
}


/* Unlike in X11, window events in Carbon or Cocoa, whose event system
   is implemented on top of Carbon, do not come from sockets.  So we
   cannot simply use `select' to monitor two kinds of inputs: window
   events and process outputs.  We emulate such functionality by
   regarding fd 0 as the window event channel and simultaneously
   monitoring both kinds of input channels.  It is implemented by
   dividing into some cases:
   1. The window event channel is not involved.
      -> Use `select'.
   2. Sockets are not involved.
      -> Run the run loop in the main thread to wait for window events
         (and user signals, see below).
   3. Otherwise
      -> Run the run loop in the main thread while calling `select' in
         a secondary thread using either Pthreads with CFRunLoopSource
         or Grand Central Dispatch (GCD, on Mac OS X 10.6 or later).
         When the control returns from the `select' call, the
         secondary thread sends a wakeup notification to the main
         thread through either a CFSocket (for Pthreads with
         CFRunLoopSource) or a dispatch source (for GCD).
   For Case 2 and 3, user signals such as SIGUSR1 are also handled
   through either a CFSocket or a dispatch source.  */

static int wakeup_fds[2];
/* Whether we have read some input from wakeup_fds[0] after resetting
   this variable.  Don't access it outside the main thread.  */
static int wokeup_from_run_loop_run_once_p;

static int
read_all_from_nonblocking_fd (fd)
     int fd;
{
  int rtnval;
  char buf[64];

  do
    {
      rtnval = read (fd, buf, sizeof (buf));
    }
  while (rtnval > 0 || (rtnval < 0 && errno == EINTR));

  return rtnval;
}

static int
write_one_byte_to_fd (fd)
     int fd;
{
  int rtnval;

  do
    {
      rtnval = write (fd, "", 1);
    }
  while (rtnval == 0 || (rtnval < 0 && errno == EINTR));

  return rtnval;
}

#if SELECT_USE_GCD
static dispatch_queue_t select_dispatch_queue;
#else
static void
wakeup_callback (s, type, address, data, info)
     CFSocketRef s;
     CFSocketCallBackType type;
     CFDataRef address;
     const void *data;
     void *info;
{
  read_all_from_nonblocking_fd (CFSocketGetNative (s));
  wokeup_from_run_loop_run_once_p = 1;
}
#endif

int
init_wakeup_fds ()
{
  int result, i;
  int flags;

  result = socketpair (AF_UNIX, SOCK_STREAM, 0, wakeup_fds);
  if (result < 0)
    return result;
  for (i = 0; i < 2; i++)
    {
      flags = fcntl (wakeup_fds[i], F_GETFL, 0);
      result = fcntl (wakeup_fds[i], F_SETFL, flags | O_NONBLOCK);
      if (result < 0)
	return result;
    }
#if SELECT_USE_GCD
  {
    dispatch_source_t source;

    source = dispatch_source_create (DISPATCH_SOURCE_TYPE_READ, wakeup_fds[0],
				     0, dispatch_get_main_queue ());
    if (source == NULL)
      return -1;
    dispatch_source_set_event_handler (source, ^{
	read_all_from_nonblocking_fd (dispatch_source_get_handle (source));
	wokeup_from_run_loop_run_once_p = 1;
      });
    dispatch_resume (source);

    select_dispatch_queue = dispatch_queue_create ("org.gnu.Emacs.select",
						   NULL);
    if (select_dispatch_queue == NULL)
      return -1;
  }
#else
  {
    CFSocketRef socket;
    CFRunLoopSourceRef source;

    socket = CFSocketCreateWithNative (NULL, wakeup_fds[0],
				       kCFSocketReadCallBack,
				       wakeup_callback, NULL);
    if (socket == NULL)
      return -1;
    source = CFSocketCreateRunLoopSource (NULL, socket, 0);
    CFRelease (socket);
    if (source == NULL)
      return -1;
    CFRunLoopAddSource ((CFRunLoopRef)
			GetCFRunLoopFromEventLoop (GetCurrentEventLoop ()),
			source, kCFRunLoopDefaultMode);
    CFRelease (source);
  }
#endif
  return 0;
}

void
mac_wakeup_from_run_loop_run_once ()
{
  /* This function may be called from a signal hander, so only
     async-signal safe functions can be used here.  */
  write_one_byte_to_fd (wakeup_fds[1]);
}

/* Return next event in the main queue if it exists.  Otherwise return
   NULL.  */

EventRef
mac_peek_next_event ()
{
  EventRef event;

#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1030
#if MAC_OS_X_VERSION_MIN_REQUIRED == 1020
  if (AcquireFirstMatchingEventInQueue != NULL)
#endif
    {
      event = AcquireFirstMatchingEventInQueue (GetCurrentEventQueue (), 0,
						NULL, kEventQueueOptionsNone);
      if (event)
	ReleaseEvent (event);
    }
#if MAC_OS_X_VERSION_MIN_REQUIRED == 1020
  else			/* AcquireFirstMatchingEventInQueue == NULL */
#endif
#endif	/* MAC_OS_X_VERSION_MAX_ALLOWED >= 1030  */
#if MAC_OS_X_VERSION_MAX_ALLOWED < 1030 || MAC_OS_X_VERSION_MIN_REQUIRED == 1020
    {
      OSStatus err;

      err = ReceiveNextEvent (0, NULL, kEventDurationNoWait,
			      kEventLeaveInQueue, &event);
      if (err != noErr)
	event = NULL;
    }
#endif

  return event;
}

#if !SELECT_USE_GCD
static struct
{
  int value;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} select_sem = {0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void
select_sem_wait ()
{
  pthread_mutex_lock (&select_sem.mutex);
  while (select_sem.value <= 0)
    pthread_cond_wait (&select_sem.cond, &select_sem.mutex);
  select_sem.value--;
  pthread_mutex_unlock (&select_sem.mutex);
}

static void
select_sem_signal ()
{
  pthread_mutex_lock (&select_sem.mutex);
  select_sem.value++;
  pthread_cond_signal (&select_sem.cond);
  pthread_mutex_unlock (&select_sem.mutex);
}

static CFRunLoopSourceRef select_run_loop_source = NULL;
static CFRunLoopRef select_run_loop = NULL;

static struct
{
  int nfds;
  SELECT_TYPE *rfds, *wfds, *efds;
  EMACS_TIME *timeout;
} select_args;

static void
select_perform (info)
     void *info;
{
  int qnfds = select_args.nfds;
  SELECT_TYPE qrfds, qwfds, qefds;
  EMACS_TIME qtimeout;
  int r;

  if (select_args.rfds)
    qrfds = *select_args.rfds;
  if (select_args.wfds)
    qwfds = *select_args.wfds;
  if (select_args.efds)
    qefds = *select_args.efds;
  if (select_args.timeout)
    qtimeout = *select_args.timeout;

  if (wakeup_fds[1] >= qnfds)
    qnfds = wakeup_fds[1] + 1;
  FD_SET (wakeup_fds[1], &qrfds);

  r = select (qnfds, select_args.rfds ? &qrfds : NULL,
	      select_args.wfds ? &qwfds : NULL,
	      select_args.efds ? &qefds : NULL,
	      select_args.timeout ? &qtimeout : NULL);
  if (r < 0 || (r > 0 && !FD_ISSET (wakeup_fds[1], &qrfds)))
    mac_wakeup_from_run_loop_run_once ();

  select_sem_signal ();
}

static void
select_fire (nfds, rfds, wfds, efds, timeout)
     int nfds;
     SELECT_TYPE *rfds, *wfds, *efds;
     EMACS_TIME *timeout;
{
  select_args.nfds = nfds;
  select_args.rfds = rfds;
  select_args.wfds = wfds;
  select_args.efds = efds;
  select_args.timeout = timeout;

  CFRunLoopSourceSignal (select_run_loop_source);
  CFRunLoopWakeUp (select_run_loop);
}

static void *
select_thread_main (arg)
     void *arg;
{
  CFRunLoopSourceContext context = {0, NULL, NULL, NULL, NULL, NULL, NULL,
				    NULL, NULL, select_perform};

  select_run_loop = CFRunLoopGetCurrent ();
  select_run_loop_source = CFRunLoopSourceCreate (NULL, 0, &context);
  CFRunLoopAddSource (select_run_loop, select_run_loop_source,
		      kCFRunLoopDefaultMode);
  select_sem_signal ();
  CFRunLoopRun ();
}

static void
select_thread_launch ()
{
  pthread_attr_t  attr;
  pthread_t       thread;

  pthread_attr_init (&attr);
  pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
  pthread_create (&thread, &attr, &select_thread_main, NULL);
}
#endif	/* !SELECT_USE_GCD */

static int
select_and_poll_event (nfds, rfds, wfds, efds, timeout)
     int nfds;
     SELECT_TYPE *rfds, *wfds, *efds;
     EMACS_TIME *timeout;
{
  int timedout_p = 0;
  int r = 0;
  EMACS_TIME select_timeout;
  EventTimeout timeoutval =
    (timeout
     ? (EMACS_SECS (*timeout) * kEventDurationSecond
	+ EMACS_USECS (*timeout) * kEventDurationMicrosecond)
     : kEventDurationForever);
  SELECT_TYPE orfds, owfds, oefds;

  if (timeout == NULL)
    {
      if (rfds) orfds = *rfds;
      if (wfds) owfds = *wfds;
      if (efds) oefds = *efds;
    }

  /* Try detect_input_pending before mac_run_loop_run_once in the same
     BLOCK_INPUT block, in case that some input has already been read
     asynchronously.  */
  BLOCK_INPUT;
  while (1)
    {
      if (detect_input_pending ())
	break;

      EMACS_SET_SECS_USECS (select_timeout, 0, 0);
      r = select (nfds, rfds, wfds, efds, &select_timeout);
      if (r != 0)
	break;

      if (timeoutval == 0.0)
	timedout_p = 1;
      else
	{
	  /* On Mac OS X 10.7, delayed visible toolbar item validation
	     (see the documentation of -[NSToolbar
	     validateVisibleItems]) is treated as if it were an input
	     source firing rather than a timer function (as in Mac OS
	     X 10.6).  So it makes -[NSRunLoop runMode:beforeDate:],
	     which is used in the implementation of
	     mac_run_loop_run_once, return despite no available input
	     to process.  In such cases, we want to call
	     mac_run_loop_run_once again so as to avoid wasting CPU
	     time caused by vacuous reactivation of delayed visible
	     toolbar item validation via window update events issued
	     in the application event loop.  */
	  do
	    {
	      timeoutval = mac_run_loop_run_once (timeoutval);
	    }
	  while (timeoutval && !mac_peek_next_event ()
		 && !detect_input_pending ());
	  if (timeoutval == 0)
	    timedout_p = 1;
	}

      if (timeout == NULL && timedout_p)
	{
	  if (rfds) *rfds = orfds;
	  if (wfds) *wfds = owfds;
	  if (efds) *efds = oefds;
	}
      else
	break;
    }
  UNBLOCK_INPUT;

  if (r != 0)
    return r;
  else if (!timedout_p)
    {
      /* Pretend that `select' is interrupted by a signal.  */
      detect_input_pending ();
      errno = EINTR;
      return -1;
    }
  else
    return 0;
}

int
sys_select (nfds, rfds, wfds, efds, timeout)
     int nfds;
     SELECT_TYPE *rfds, *wfds, *efds;
     EMACS_TIME *timeout;
{
  int timedout_p = 0;
  int r;
  EMACS_TIME select_timeout;
  SELECT_TYPE orfds, owfds, oefds;
  EventTimeout timeoutval;

  if (inhibit_window_system || noninteractive
      || nfds < 1 || rfds == NULL || !FD_ISSET (0, rfds))
    return select (nfds, rfds, wfds, efds, timeout);

  FD_CLR (0, rfds);
  orfds = *rfds;

  if (wfds)
    owfds = *wfds;
  else
    FD_ZERO (&owfds);

  if (efds)
    oefds = *efds;
  else
    FD_ZERO (&oefds);

  timeoutval = (timeout
		? (EMACS_SECS (*timeout) * kEventDurationSecond
		   + EMACS_USECS (*timeout) * kEventDurationMicrosecond)
		: kEventDurationForever);

  FD_SET (0, rfds);		/* sentinel */
  do
    {
      nfds--;
    }
  while (!(FD_ISSET (nfds, rfds) || (wfds && FD_ISSET (nfds, wfds))
	   || (efds && FD_ISSET (nfds, efds))));
  nfds++;
  FD_CLR (0, rfds);

  if (nfds == 1)
    return select_and_poll_event (nfds, rfds, wfds, efds, timeout);

  /* Avoid initial overhead of RunLoop setup for the case that some
     input is already available.  */
  EMACS_SET_SECS_USECS (select_timeout, 0, 0);
  r = select_and_poll_event (nfds, rfds, wfds, efds, &select_timeout);
  if (r != 0 || timeoutval == 0.0)
    return r;

  *rfds = orfds;
  if (wfds)
    *wfds = owfds;
  if (efds)
    *efds = oefds;

  /* Try detect_input_pending before mac_run_loop_run_once in the same
     BLOCK_INPUT block, in case that some input has already been read
     asynchronously.  */
  BLOCK_INPUT;
  if (!detect_input_pending ())
    {
#if SELECT_USE_GCD
      dispatch_sync (select_dispatch_queue, ^{});
      wokeup_from_run_loop_run_once_p = 0;
      dispatch_async (select_dispatch_queue, ^{
	  SELECT_TYPE qrfds = orfds, qwfds = owfds, qefds = oefds;
	  int qnfds = nfds;
	  int r;

	  if (wakeup_fds[1] >= qnfds)
	    qnfds = wakeup_fds[1] + 1;
	  FD_SET (wakeup_fds[1], &qrfds);

	  r = select (qnfds, &qrfds, wfds ? &qwfds : NULL,
		      efds ? &qefds : NULL, NULL);
	  if (r < 0 || (r > 0 && !FD_ISSET (wakeup_fds[1], &qrfds)))
	    mac_wakeup_from_run_loop_run_once ();
	});

      do
	{
	  timeoutval = mac_run_loop_run_once (timeoutval);
	}
      while (timeoutval && !wokeup_from_run_loop_run_once_p
	     && !mac_peek_next_event () && !detect_input_pending ());
      if (timeoutval == 0)
	timedout_p = 1;

      write_one_byte_to_fd (wakeup_fds[0]);
      dispatch_async (select_dispatch_queue, ^{
	  read_all_from_nonblocking_fd (wakeup_fds[1]);
	});
#else
      if (select_run_loop == NULL)
	select_thread_launch ();

      select_sem_wait ();
      read_all_from_nonblocking_fd (wakeup_fds[1]);
      wokeup_from_run_loop_run_once_p = 0;
      select_fire (nfds, rfds, wfds, efds, NULL);

      do
	{
	  timeoutval = mac_run_loop_run_once (timeoutval);
	}
      while (timeoutval && !wokeup_from_run_loop_run_once_p
	     && !mac_peek_next_event () && !detect_input_pending ());
      if (timeoutval == 0)
	timedout_p = 1;

      write_one_byte_to_fd (wakeup_fds[0]);
#endif
    }
  UNBLOCK_INPUT;

  if (!timedout_p)
    {
      EMACS_SET_SECS_USECS (select_timeout, 0, 0);
      r = select_and_poll_event (nfds, rfds, wfds, efds, &select_timeout);
      if (r != 0)
	return r;
      errno = EINTR;
      return -1;
    }
  else
    {
      FD_ZERO (rfds);
      if (wfds)
	FD_ZERO (wfds);
      if (efds)
	FD_ZERO (efds);
      return 0;
    }
}

/* Return whether the service provider for the current application is
   already registered.  */

int
mac_service_provider_registered_p ()
{
  name_t name = "org.gnu.Emacs";
  CFBundleRef bundle;
  mach_port_t port;
  kern_return_t kr;

  bundle = CFBundleGetMainBundle ();
  if (bundle)
    {
      CFStringRef identifier = CFBundleGetIdentifier (bundle);

      if (identifier)
	CFStringGetCString (identifier, name, sizeof (name),
			    kCFStringEncodingUTF8);
    }
  strlcat (name, ".ServiceProvider", sizeof (name));
  kr = bootstrap_look_up (bootstrap_port, name, &port);
  if (kr == KERN_SUCCESS)
    mach_port_deallocate (mach_task_self (), port);

  return kr == KERN_SUCCESS;
}

/* Set up environment variables so that Emacs can correctly find its
   support files when packaged as an application bundle.  Directories
   placed in /usr/local/share/emacs/<emacs-version>/, /usr/local/bin,
   and /usr/local/libexec/emacs/<emacs-version>/<system-configuration>
   by `make install' by default can instead be placed in
   .../Emacs.app/Contents/Resources/ and
   .../Emacs.app/Contents/MacOS/.  Each of these environment variables
   is changed only if it is not already set.  Presumably if the user
   sets an environment variable, he will want to use files in his path
   instead of ones in the application bundle.  */
void
init_mac_osx_environment ()
{
  CFBundleRef bundle;
  CFURLRef bundleURL;
  CFStringRef cf_app_bundle_pathname;
  int app_bundle_pathname_len;
  char *app_bundle_pathname;
  char *p, *q;
  struct stat st;

  /* Initialize locale related variables.  */
  mac_system_script_code = mac_get_system_script_code ();
  Vmac_system_locale = IS_DAEMON ? Qnil : mac_get_system_locale ();

  /* Fetch the pathname of the application bundle as a C string into
     app_bundle_pathname.  */

  bundle = CFBundleGetMainBundle ();
  if (!bundle || CFBundleGetIdentifier (bundle) == NULL)
    {
      /* We could not find the bundle identifier.  For now, prevent
	 the fatal error by bringing it up in the terminal. */
      inhibit_window_system = 1;
      return;
    }

  bundleURL = CFBundleCopyBundleURL (bundle);
  if (!bundleURL)
    return;

  cf_app_bundle_pathname = CFURLCopyFileSystemPath (bundleURL,
						    kCFURLPOSIXPathStyle);
  CFRelease (bundleURL);
  {
    Lisp_Object temp = cfstring_to_lisp_nodecode (cf_app_bundle_pathname);

    app_bundle_pathname_len = SBYTES (temp);
    app_bundle_pathname = SDATA (temp);
  }

  CFRelease (cf_app_bundle_pathname);

  /* P should have sufficient room for the pathname of the bundle plus
     the subpath in it leading to the respective directories.  Q
     should have three times that much room because EMACSLOADPATH can
     have the value "<path to site-lisp dir>:<path to lisp dir>:<path
     to leim dir>".  */
  p = (char *) alloca (app_bundle_pathname_len + 50);
  q = (char *) alloca (3 * app_bundle_pathname_len + 150);
  if (!getenv ("EMACSLOADPATH"))
    {
      q[0] = '\0';

      strcpy (p, app_bundle_pathname);
      strcat (p, "/Contents/Resources/site-lisp");
      if (stat (p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR)
	strcat (q, p);

      strcpy (p, app_bundle_pathname);
      strcat (p, "/Contents/Resources/lisp");
      if (stat (p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR)
	{
	  if (q[0] != '\0')
	    strcat (q, ":");
	  strcat (q, p);
	}

      strcpy (p, app_bundle_pathname);
      strcat (p, "/Contents/Resources/leim");
      if (stat (p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR)
	{
	  if (q[0] != '\0')
	    strcat (q, ":");
	  strcat (q, p);
	}

      if (q[0] != '\0')
	setenv ("EMACSLOADPATH", q, 1);
    }

  if (!getenv ("EMACSPATH"))
    {
      q[0] = '\0';

      strcpy (p, app_bundle_pathname);
      strcat (p, "/Contents/MacOS/libexec");
      if (stat (p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR)
	strcat (q, p);

      strcpy (p, app_bundle_pathname);
      strcat (p, "/Contents/MacOS/bin");
      if (stat (p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR)
	{
	  if (q[0] != '\0')
	    strcat (q, ":");
	  strcat (q, p);
	}

      if (q[0] != '\0')
	setenv ("EMACSPATH", q, 1);
    }

  if (!getenv ("EMACSDATA"))
    {
      strcpy (p, app_bundle_pathname);
      strcat (p, "/Contents/Resources/etc");
      if (stat (p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR)
	setenv ("EMACSDATA", p, 1);
    }

  if (!getenv ("EMACSDOC"))
    {
      strcpy (p, app_bundle_pathname);
      strcat (p, "/Contents/Resources/etc");
      if (stat (p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR)
	setenv ("EMACSDOC", p, 1);
    }

  if (!getenv ("INFOPATH"))
    {
      strcpy (p, app_bundle_pathname);
      strcat (p, "/Contents/Resources/info");
      if (stat (p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR)
	setenv ("INFOPATH", p, 1);
    }

  if (IS_DAEMON)
    inhibit_window_system = 1;
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 1030
  else
    {
      /* AVAILABLE_MAC_OS_X_VERSION_10_3_AND_LATER is missing in some SDKs.  */
      CG_EXTERN CFDictionaryRef CGSessionCopyCurrentDictionary(void)  AVAILABLE_MAC_OS_X_VERSION_10_3_AND_LATER;

#if MAC_OS_X_VERSION_MIN_REQUIRED == 1020
      if (CGSessionCopyCurrentDictionary != NULL)
#endif
	{
	  CFDictionaryRef session_dict = CGSessionCopyCurrentDictionary ();

	  if (session_dict == NULL)
	    /* No window server session.  */
	    inhibit_window_system = 1;
	  else
	    CFRelease (session_dict);
	}
    }
#endif
}


void
syms_of_mac ()
{
  Qundecoded_file_name = intern_c_string ("undecoded-file-name");
  staticpro (&Qundecoded_file_name);

  Qstring  = intern_c_string ("string");	staticpro (&Qstring);
  Qnumber  = intern_c_string ("number");	staticpro (&Qnumber);
  Qboolean = intern_c_string ("boolean");	staticpro (&Qboolean);
  Qdate	   = intern_c_string ("date");		staticpro (&Qdate);
  Qarray   = intern_c_string ("array");		staticpro (&Qarray);
  Qdictionary = intern_c_string ("dictionary");	staticpro (&Qdictionary);
  Qrange = intern_c_string ("range");		staticpro (&Qrange);
  Qpoint = intern_c_string ("point");		staticpro (&Qpoint);
  Qdescription = intern_c_string ("description"); staticpro (&Qdescription);

  Qmac_file_alias_p = intern_c_string ("mac-file-alias-p");
  staticpro (&Qmac_file_alias_p);

  Qxml = intern_c_string ("xml");
  staticpro (&Qxml);
  Qxml1 = intern_c_string ("xml1");
  staticpro (&Qxml1);
  Qbinary1 = intern_c_string ("binary1");
  staticpro (&Qbinary1);

  QCmime_charset = intern_c_string (":mime-charset");
  staticpro (&QCmime_charset);

  QNFD  = intern_c_string ("NFD");		staticpro (&QNFD);
  QNFKD = intern_c_string ("NFKD");		staticpro (&QNFKD);
  QNFC  = intern_c_string ("NFC");		staticpro (&QNFC);
  QNFKC = intern_c_string ("NFKC");		staticpro (&QNFKC);
  QHFS_plus_D = intern_c_string ("HFS+D");	staticpro (&QHFS_plus_D);
  QHFS_plus_C = intern_c_string ("HFS+C");	staticpro (&QHFS_plus_C);

  {
    int i;

    for (i = 0; i < sizeof (ae_attr_table) / sizeof (ae_attr_table[0]); i++)
      {
	ae_attr_table[i].symbol = intern_c_string (ae_attr_table[i].name);
	staticpro (&ae_attr_table[i].symbol);
      }
  }

  defsubr (&Smac_coerce_ae_data);
  defsubr (&Smac_get_preference);
  defsubr (&Smac_convert_property_list);
  defsubr (&Smac_code_convert_string);
  defsubr (&Smac_process_hi_command);

  defsubr (&Smac_set_file_creator);
  defsubr (&Smac_set_file_type);
  defsubr (&Smac_get_file_creator);
  defsubr (&Smac_get_file_type);
  defsubr (&Smac_file_alias_p);
  defsubr (&Ssystem_move_file_to_trash);
  defsubr (&Sdo_applescript);

  DEFVAR_INT ("mac-system-script-code", &mac_system_script_code,
    doc: /* The system script code.  */);
  mac_system_script_code = mac_get_system_script_code ();

  DEFVAR_LISP ("mac-system-locale", &Vmac_system_locale,
    doc: /* The system locale identifier string.
This is not a POSIX locale ID, but an ICU locale ID.  So encoding
information is not included.  */);
  Vmac_system_locale = mac_get_system_locale ();
}

/* arch-tag: 29d30c1f-0c6b-4f88-8a6d-0558d7f9dbff
   (do not change this comment) */
