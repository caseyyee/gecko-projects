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
 *
 * Created: Jamie Zawinski <jwz@netscape.com>, 28 Aug 1997.
 */

package grendel.mime.encoder;

/** This abstract class is the parent of those classes which implement
    MIME encoding and decoding: base64, quoted-printable, and uuencode.
    @see MimeBase64Encoder
    @see MimeBase64Decoder
    @see MimeQuotedPrintableEncoder
    @see MimeQuotedPrintableDecoder
    @see MimeUUEncoder
    @see MimeUUDecoder
 */

import calypso.util.ByteBuf;

public abstract class MimeEncoder {

  /** Given a sequence of input bytes, produces a sequence of output bytes.
      Note that some (small) amount of buffering may be necessary, if the
      input byte stream didn't fall on an appropriate boundary.  If there
      are bytes in `out' already, the new bytes are appended, so the
      caller should do `out.setLength(0)' first if that's desired.
   */
  abstract public void translate(ByteBuf in, ByteBuf out);

  /** Tell the decoder that no more input data will be forthcoming.
      This may result in output, as a result of flushing the internal
      buffer.  This object must not be used again after calling eof().
      If there are bytes in `out' already, the new bytes are appended,
      so the caller should do `out.setLength(0)' first if that's desired.
   */
  abstract public void eof(ByteBuf out);
}
