/* -*- Mode: java; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.0 (the "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied.  See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the Grendel mail/news client.
 *
 * The Initial Developer of the Original Code is Netscape Communications
 * Corporation.  Portions created by Netscape are Copyright (C) 1997
 * Netscape Communications Corporation.  All Rights Reserved.
 */

package grendel;

import calypso.util.Preferences;
import calypso.util.PreferencesFactory;

import grendel.ui.MessageDisplayManager;
import grendel.ui.MultiMessageDisplayManager;
import grendel.ui.UnifiedMessageDisplayManager;

/**
 * This launches the Grendel GUI.
 */

public class Main {
  static MessageDisplayManager fManager;

  public static void main(String argv[]) {
    Preferences prefs = PreferencesFactory.Get();
    String pref = prefs.getString("mail.layout", "multi_pane");

    if (pref.equals("multi_pane")) {
      fManager = new UnifiedMessageDisplayManager();
    } else {
      fManager = new MultiMessageDisplayManager();
    }
    MessageDisplayManager.SetDefaultManager(fManager);
    fManager.displayMaster();
  }
}

