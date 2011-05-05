base32_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
# calculate the reverse mapping

if __name__ == '__main__':
    binary_to_ascii_base32_table=map(lambda x: base32_alphabet[x % 32], range(256))
    ascii_to_binary_base32_table=range(256)
    #~ print repr(base32_to_ascii_table)
    for ascii in range(256):
        try:
            binary = base32_alphabet.index(chr(ascii).upper())
        except ValueError: # invalid character for base32
            binary = 0
        ascii_to_binary_base32_table[ascii] = binary

    # output C definitions
    print 'static unsigned char binary_to_ascii_base32_table[] = {',
    for ascii,binary in enumerate(binary_to_ascii_base32_table):
        if ascii % 8 == 0:
            print # line breaks every x chars
        print "\t'%s'" % binary,
        if ascii == len(binary_to_ascii_base32_table) - 1: # final char
            print
            print "};"
        else:
            print ",",

    print 'static unsigned char ascii_to_binary_base32_table[] = {',
    for binary,ascii in enumerate(ascii_to_binary_base32_table):
        # line breaks every x chars
        if binary % 8 == 0:
            print
        print "\t%s" % ascii,
        if binary == len(ascii_to_binary_base32_table) - 1: # final char
            print
            print "};"
        else:
            print ",",
