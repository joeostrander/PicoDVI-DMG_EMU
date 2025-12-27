#include "colors.h"

static const color_scheme_t color_schemes[NUMBER_OF_SCHEMES] = 
{
    [SCHEME_BLACK_AND_WHITE] =  { 0xF7F3F7, 0x4E4C4E, 0xB5B2B5, 0x000000 },
    [SCHEME_INVERTED] =         { 0x000000, 0xB5B2B5, 0x4E4C4E, 0xF7F3F7 },
    [SCHEME_DMG] =              { 0x7B8210, 0x39594A, 0x5A7942, 0x294139 },
    [SCHEME_GAME_BOY_POCKET] =  { 0xC6CBA5, 0x4A5139, 0x8C926B, 0x181818 },
    [SCHEME_GAME_BOY_LIGHT] =   { 0x00B284, 0x00694A, 0x8C926B, 0x005139 },
    [SCHEME_SGB_1A] =           { 0xF7E3C6, 0xA52821, 0xD6924A, 0x311852 },
    [SCHEME_SGB_1B] =           { 0xD6D3BD, 0xAD5110, 0xC6AA73, 0x000000 },
    [SCHEME_SGB_1C] =           { 0xF7BAF7, 0x943863, 0xE79252, 0x393894 },
    [SCHEME_SGB_1D] =           { 0xF7F3A5, 0xF70000, 0xBD824A, 0x521800 },
    [SCHEME_SGB_1E] =           { 0xF7D3AD, 0x6B8A42, 0x7BBA7B, 0x5A3821 },
    [SCHEME_SGB_1F] =           { 0xD6E3F7, 0xA50000, 0xDE8A52, 0x004110 },
    [SCHEME_SGB_1G] =           { 0x000052, 0x7B7900, 0x009AE7, 0xF7F35A },
    [SCHEME_SGB_1H] =           { 0xF7E3DE, 0x844100, 0xF7B28C, 0x311800 },
    [SCHEME_SGB_2A] =           { 0xEFC39C, 0x297900, 0xBD8A4A, 0x000000 },
    [SCHEME_SGB_2B] =           { 0xF7F3F7, 0xF73000, 0xF7E352, 0x52005A },
    [SCHEME_SGB_2C] =           { 0xF7F3F7, 0x7B30E7, 0xE78A8C, 0x292894 },
    [SCHEME_SGB_2D] =           { 0xF7F39C, 0xF73000, 0x00F300, 0x000052 },
    [SCHEME_SGB_2E] =           { 0xF7C384, 0x291063, 0x94AADE, 0x100810 },
    [SCHEME_SGB_2F] =           { 0xCEF3F7, 0x9C0000, 0xF79252, 0x180000 },
    [SCHEME_SGB_2G] =           { 0x6BB239, 0xDEB284, 0xDE5142, 0x001800 },
    [SCHEME_SGB_2H] =           { 0xF7F3F7, 0x737173, 0xB5B2B5, 0x000000 },
    [SCHEME_SGB_3A] =           { 0xF7CB94, 0xF76129, 0x73BABD, 0x314963 },
    [SCHEME_SGB_3B] =           { 0xD6D3BD, 0x005100, 0xDE8221, 0x001010 },
    [SCHEME_SGB_3C] =           { 0xDEA2C6, 0x00B2F7, 0xF7F37B, 0x21205A },
    [SCHEME_SGB_3D] =           { 0xEFF3B5, 0x96AD52, 0xDEA27B, 0x000000 },
    [SCHEME_SGB_3E] =           { 0xF7F3BD, 0xAD7921, 0xDEAA6B, 0x524973 },
    [SCHEME_SGB_3F] =           { 0x7B79C6, 0xF7CB00, 0xF769F7, 0x424142 },
    [SCHEME_SGB_3G] =           { 0x63D352, 0xC63039, 0xF7F3F7, 0x390000 },
    [SCHEME_SGB_3H] =           { 0xDEF39C, 0x4A8A18, 0x7BC339, 0x081800 },
    [SCHEME_SGB_4A] =           { 0xEFA26B, 0xCE00CE, 0x7BA2F7, 0x00007B },
    [SCHEME_SGB_4B] =           { 0xEFE3EF, 0x427939, 0xE79A63, 0x180808 },
    [SCHEME_SGB_4C] =           { 0xF7DBDE, 0x949ADE, 0xF7F37B, 0x080000 },
    [SCHEME_SGB_4D] =           { 0xF7F3B5, 0x4A697B, 0x94C3C6, 0x08204A },
    [SCHEME_SGB_4E] =           { 0xF7D3A5, 0x7B598C, 0xDEA27B, 0x002031 },
    [SCHEME_SGB_4F] =           { 0xB5CBCE, 0x84009C, 0xD682D6, 0x390000 },
    [SCHEME_SGB_4G] =           { 0xADDB18, 0x291000, 0xB5205A, 0x008263 },
    [SCHEME_SGB_4H] =           { 0xF7F3C6, 0x848A42, 0xB5BA5A, 0x425129 }
};

static int color_scheme_index = SCHEME_BLACK_AND_WHITE;

//**********************************************************************************************
// PUBLIC FUNCTIONS
//**********************************************************************************************
const color_scheme_t* get_scheme(void)
{
    return &color_schemes[color_scheme_index];
}

int get_scheme_index(void)
{
    return color_scheme_index;
}

void set_scheme_index(int index)
{
    index = index >= NUMBER_OF_SCHEMES ? 0 : index;
    index = index < 0 ? (NUMBER_OF_SCHEMES-1) : index;
    color_scheme_index = index;
}
