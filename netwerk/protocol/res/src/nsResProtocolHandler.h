/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Darin Fisher <darin@netscape.com>
 *   Benjamin Smedberg <bsmedberg@covad.net>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef nsResProtocolHandler_h___
#define nsResProtocolHandler_h___

#include "nsIResProtocolHandler.h"
#include "nsInterfaceHashtable.h"
#include "nsISupportsArray.h"
#include "nsIIOService.h"
#include "nsWeakReference.h"
#include "nsStandardURL.h"

struct ResourceMapping;

// nsResURL : overrides nsStandardURL::GetFile to provide nsIFile resolution
class nsResURL : public nsStandardURL
{
public:
    nsResURL() : nsStandardURL(PR_TRUE) {}
    virtual nsStandardURL* StartClone();
    virtual nsresult EnsureFile();
    NS_IMETHOD GetClassIDNoAlloc(nsCID *aCID);
};

class nsResProtocolHandler : public nsIResProtocolHandler, public nsSupportsWeakReference
{
public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIPROTOCOLHANDLER
    NS_DECL_NSIRESPROTOCOLHANDLER

    nsResProtocolHandler();
    virtual ~nsResProtocolHandler();

    nsresult Init();

#ifdef MOZ_IPC    
    void CollectSubstitutions(nsTArray<ResourceMapping>& aResources);
#endif

private:
    nsresult AddSpecialDir(const char* aSpecialDir, const nsACString& aSubstitution);
    nsInterfaceHashtable<nsCStringHashKey,nsIURI> mSubstitutions;
    nsCOMPtr<nsIIOService> mIOService;

    friend class nsResURL;
};

#endif /* nsResProtocolHandler_h___ */
