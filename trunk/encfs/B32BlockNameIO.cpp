/*****************************************************************************
 * Author:   Valient Gough <vgough@pobox.com>
 *
 *****************************************************************************
 * Copyright (c) 2004, Valient Gough
 *
 * This library is free software; you can distribute it and/or modify it under
 * the terms of the GNU General Public License (GPL), as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GPL in the file COPYING for more
 * details.
 */

#include "B32BlockNameIO.h"

#include "Cipher.h"
#include "base64.h"

#include <cstring>
#include <rlog/rlog.h>
#include <rlog/Error.h>
#include <rlog/RLogChannel.h>

#include "i18n.h"

using namespace rlog;
using namespace rel;
using namespace boost;

static RLogChannel * Info = DEF_CHANNEL( "info/nameio", Log_Info );


static shared_ptr<NameIO> NewB32BlockNameIO( const Interface &iface,
	const shared_ptr<Cipher> &cipher, const CipherKey &key )
{
    int blockSize = 8;
    if(cipher)
	blockSize = cipher->cipherBlockSize();

    return shared_ptr<NameIO>( new B32BlockNameIO( iface, cipher, key, blockSize));
}

static bool B32BlockIO_registered = NameIO::Register("B32Block",
	// description of block name encoding algorithm..
	// xgroup(setup)
	gettext_noop("Block encoding, hides file name size somewhat, using Base32 encoding (for case insensitive filesystems)"),
	B32BlockNameIO::CurrentInterface(),
	NewB32BlockNameIO);

/*
    - Version 1.0 computed MAC over the filename, but not the padding bytes.
      This version was from pre-release 1.1, never publically released, so no
      backward compatibility necessary.

    - Version 2.0 includes padding bytes in MAC computation.  This way the MAC
      computation uses the same number of bytes regardless of the number of
      padding bytes.

    - Version 3.0 uses full 64 bit initialization vector during IV chaining.
      Prior versions used only the output from the MAC_16 call, giving a 1 in
      2^16 chance of the same name being produced.  Using the full 64 bit IV
      changes that to a 1 in 2^64 chance..
*/
Interface B32BlockNameIO::CurrentInterface()
{
    // implement major version 3 and 2
    return Interface("nameio/b32block", 3, 0, 1);
}

B32BlockNameIO::B32BlockNameIO( const rel::Interface &iface,
	const shared_ptr<Cipher> &cipher,
	const CipherKey &key, int blockSize )
    : _interface( iface.current() )
    , _bs( blockSize )
    , _cipher( cipher )
    , _key( key )
{
    // just to be safe..
    rAssert( blockSize < 128 );
}

B32BlockNameIO::~B32BlockNameIO()
{
}

Interface B32BlockNameIO::interface() const
{
    return CurrentInterface();
}

int B32BlockNameIO::maxEncodedNameLen( int plaintextNameLen ) const
{
    // number of blocks, rounded up.. Only an estimate at this point, err on
    // the size of too much space rather then too little.
    int numBlocks = ( plaintextNameLen + _bs ) / _bs;
    int encodedNameLen = numBlocks * _bs + 2; // 2 checksum bytes
    return B256ToB32Bytes( encodedNameLen );
}

int B32BlockNameIO::maxDecodedNameLen( int encodedNameLen ) const
{
    int decLen256 = B32ToB256Bytes( encodedNameLen );
    return decLen256 - 2; // 2 checksum bytes removed..
}

int B32BlockNameIO::encodeName( const char *plaintextName, int length,
	uint64_t *iv, char *encodedName ) const
{
    // copy the data into the encoding buffer..
    memcpy( encodedName+2, plaintextName, length );

    // Pad encryption buffer to block boundary..
    int padding = _bs - length % _bs;
    if(padding == 0)
	padding = _bs; // padding a full extra block!

    memset( encodedName+length+2, (unsigned char)padding, padding );

    // store the IV before it is modified by the MAC call.
    uint64_t tmpIV = 0;
    if( iv && _interface >= 3 )
	tmpIV = *iv;

    // include padding in MAC computation
    unsigned int mac = _cipher->MAC_16( (unsigned char *)encodedName+2,
	    length+padding, _key, iv );

    // add checksum bytes
    encodedName[0] = (mac >> 8) & 0xff;
    encodedName[1] = (mac     ) & 0xff;

    _cipher->blockEncode( (unsigned char *)encodedName+2, length+padding,
	    (uint64_t)mac ^ tmpIV, _key);

    // convert to base 64 ascii
    int encodedStreamLen = length + 2 + padding;
    int encLen32 = B256ToB32Bytes( encodedStreamLen );

    changeBase2Inline( (unsigned char *)encodedName, encodedStreamLen,
	    8, 5, true );
    B32ToAscii( (unsigned char *)encodedName, encLen32 );

    return encLen32;
}

int B32BlockNameIO::decodeName( const char *encodedName, int length,
	uint64_t *iv, char *plaintextName ) const
{
    int decLen256 = B32ToB256Bytes( length );
    int decodedStreamLen = decLen256 - 2;

    // don't bother trying to decode files which are too small
    if(decodedStreamLen < _bs)
	throw ERROR("Filename too small to decode");

    BUFFER_INIT( tmpBuf, 32, (unsigned int)length );

    // decode into tmpBuf,
    AsciiToB32((unsigned char *)tmpBuf, (unsigned char *)encodedName, length);
    changeBase2Inline((unsigned char *)tmpBuf, length, 5, 8, false);

    // pull out the header information
    unsigned int mac = ((unsigned int)((unsigned char)tmpBuf[0])) << 8
	             | ((unsigned int)((unsigned char)tmpBuf[1]));

    uint64_t tmpIV = 0;
    if( iv && _interface >= 3 )
	tmpIV = *iv;

    _cipher->blockDecode( (unsigned char *)tmpBuf+2, decodedStreamLen,
	    (uint64_t)mac ^ tmpIV, _key);

    // find out true string length
    int padding = (unsigned char)tmpBuf[2+decodedStreamLen-1];
    int finalSize = decodedStreamLen - padding;

    // might happen if there is an error decoding..
    if(padding > _bs || finalSize < 0)
    {
	rDebug("padding, _bx, finalSize = %i, %i, %i", padding,
		_bs, finalSize);
	throw ERROR( "invalid padding size" );
    }

    // copy out the result..
    memcpy(plaintextName, tmpBuf+2, finalSize);
    plaintextName[finalSize] = '\0';

    // check the mac
    unsigned int mac2 = _cipher->MAC_16((const unsigned char *)tmpBuf+2,
	    decodedStreamLen, _key, iv);

    BUFFER_RESET( tmpBuf );

    if(mac2 != mac)
    {
	rDebug("checksum mismatch: expected %u, got %u", mac, mac2);
	rDebug("on decode of %i bytes", finalSize);
	throw ERROR( "checksum mismatch in filename decode" );
    }

    return finalSize;
}

bool B32BlockNameIO::Enabled()
{
    return true;
}
