#include "sd_card.h"
#include "xm_file.h"
#include "ui_tap.h"
#include "pins.h" // SAMPLE_RATE
#include <string.h>

namespace {

// FT2's linear frequency table: 8363 * 64 * 2^(-i/768) Hz.
const uint32_t LINEAR_FREQ[768] = {
    535232, 534749, 534267, 533785, 533303, 532822, 532341, 531861, 531381, 530902,
    530423, 529945, 529466, 528989, 528512, 528035, 527558, 527083, 526607, 526132,
    525657, 525183, 524709, 524236, 523763, 523291, 522819, 522347, 521876, 521405,
    520934, 520465, 519995, 519526, 519057, 518589, 518121, 517654, 517187, 516720,
    516254, 515788, 515323, 514858, 514394, 513930, 513466, 513003, 512540, 512078,
    511616, 511154, 510693, 510232, 509772, 509312, 508853, 508394, 507935, 507477,
    507019, 506561, 506104, 505648, 505192, 504736, 504281, 503826, 503371, 502917,
    502463, 502010, 501557, 501105, 500653, 500201, 499750, 499299, 498849, 498399,
    497949, 497500, 497051, 496602, 496154, 495707, 495260, 494813, 494367, 493921,
    493475, 493030, 492585, 492141, 491697, 491253, 490810, 490367, 489925, 489483,
    489041, 488600, 488159, 487719, 487279, 486839, 486400, 485961, 485523, 485085,
    484647, 484210, 483773, 483337, 482901, 482465, 482030, 481595, 481161, 480727,
    480293, 479860, 479427, 478994, 478562, 478130, 477699, 477268, 476838, 476407,
    475978, 475548, 475119, 474691, 474262, 473834, 473407, 472980, 472553, 472127,
    471701, 471275, 470850, 470426, 470001, 469577, 469154, 468730, 468307, 467885,
    467463, 467041, 466620, 466199, 465778, 465358, 464938, 464519, 464100, 463681,
    463263, 462845, 462427, 462010, 461593, 461177, 460761, 460345, 459930, 459515,
    459101, 458686, 458273, 457859, 457446, 457033, 456621, 456209, 455798, 455386,
    454976, 454565, 454155, 453745, 453336, 452927, 452519, 452110, 451702, 451295,
    450888, 450481, 450075, 449669, 449263, 448858, 448453, 448048, 447644, 447240,
    446837, 446434, 446031, 445628, 445226, 444825, 444424, 444023, 443622, 443222,
    442822, 442423, 442023, 441625, 441226, 440828, 440430, 440033, 439636, 439240,
    438843, 438447, 438052, 437657, 437262, 436867, 436473, 436080, 435686, 435293,
    434900, 434508, 434116, 433725, 433333, 432942, 432552, 432162, 431772, 431382,
    430993, 430604, 430216, 429828, 429440, 429052, 428665, 428279, 427892, 427506,
    427121, 426735, 426350, 425966, 425581, 425198, 424814, 424431, 424048, 423665,
    423283, 422901, 422520, 422139, 421758, 421377, 420997, 420617, 420238, 419859,
    419480, 419102, 418723, 418346, 417968, 417591, 417215, 416838, 416462, 416086,
    415711, 415336, 414961, 414587, 414213, 413839, 413466, 413093, 412720, 412348,
    411976, 411604, 411233, 410862, 410491, 410121, 409751, 409381, 409012, 408643,
    408274, 407906, 407538, 407171, 406803, 406436, 406070, 405703, 405337, 404972,
    404606, 404241, 403877, 403512, 403148, 402784, 402421, 402058, 401695, 401333,
    400971, 400609, 400248, 399887, 399526, 399166, 398805, 398446, 398086, 397727,
    397368, 397010, 396652, 396294, 395936, 395579, 395222, 394866, 394510, 394154,
    393798, 393443, 393088, 392733, 392379, 392025, 391671, 391318, 390965, 390612,
    390260, 389908, 389556, 389205, 388854, 388503, 388152, 387802, 387452, 387103,
    386754, 386405, 386056, 385708, 385360, 385012, 384665, 384318, 383971, 383625,
    383279, 382933, 382587, 382242, 381897, 381553, 381209, 380865, 380521, 380178,
    379835, 379492, 379150, 378808, 378466, 378125, 377784, 377443, 377102, 376762,
    376422, 376083, 375743, 375404, 375066, 374727, 374389, 374052, 373714, 373377,
    373040, 372704, 372367, 372032, 371696, 371361, 371026, 370691, 370356, 370022,
    369689, 369355, 369022, 368689, 368356, 368024, 367692, 367360, 367029, 366698,
    366367, 366036, 365706, 365376, 365047, 364717, 364388, 364060, 363731, 363403,
    363075, 362748, 362420, 362094, 361767, 361440, 361114, 360789, 360463, 360138,
    359813, 359489, 359164, 358840, 358516, 358193, 357870, 357547, 357225, 356902,
    356580, 356259, 355937, 355616, 355295, 354975, 354655, 354335, 354015, 353696,
    353376, 353058, 352739, 352421, 352103, 351785, 351468, 351151, 350834, 350518,
    350201, 349886, 349570, 349255, 348939, 348625, 348310, 347996, 347682, 347368,
    347055, 346742, 346429, 346117, 345804, 345492, 345181, 344869, 344558, 344247,
    343937, 343627, 343317, 343007, 342697, 342388, 342079, 341771, 341462, 341154,
    340847, 340539, 340232, 339925, 339618, 339312, 339006, 338700, 338394, 338089,
    337784, 337479, 337175, 336871, 336567, 336263, 335960, 335657, 335354, 335052,
    334749, 334447, 334146, 333844, 333543, 333242, 332941, 332641, 332341, 332041,
    331742, 331442, 331143, 330845, 330546, 330248, 329950, 329652, 329355, 329058,
    328761, 328464, 328168, 327872, 327576, 327281, 326986, 326691, 326396, 326101,
    325807, 325513, 325220, 324926, 324633, 324340, 324048, 323755, 323463, 323171,
    322880, 322589, 322298, 322007, 321716, 321426, 321136, 320846, 320557, 320268,
    319979, 319690, 319402, 319114, 318826, 318538, 318251, 317964, 317677, 317390,
    317104, 316818, 316532, 316247, 315961, 315676, 315391, 315107, 314823, 314539,
    314255, 313971, 313688, 313405, 313122, 312840, 312558, 312276, 311994, 311713,
    311431, 311150, 310870, 310589, 310309, 310029, 309749, 309470, 309191, 308912,
    308633, 308355, 308077, 307799, 307521, 307244, 306966, 306690, 306413, 306136,
    305860, 305584, 305309, 305033, 304758, 304483, 304208, 303934, 303660, 303386,
    303112, 302839, 302566, 302293, 302020, 301747, 301475, 301203, 300932, 300660,
    300389, 300118, 299847, 299577, 299306, 299036, 298767, 298497, 298228, 297959,
    297690, 297421, 297153, 296885, 296617, 296350, 296082, 295815, 295548, 295282,
    295015, 294749, 294483, 294218, 293952, 293687, 293422, 293157, 292893, 292629,
    292365, 292101, 291837, 291574, 291311, 291048, 290786, 290523, 290261, 289999,
    289738, 289476, 289215, 288954, 288694, 288433, 288173, 287913, 287653, 287394,
    287135, 286876, 286617, 286358, 286100, 285842, 285584, 285326, 285069, 284812,
    284555, 284298, 284042, 283785, 283529, 283273, 283018, 282763, 282508, 282253,
    281998, 281744, 281489, 281236, 280982, 280728, 280475, 280222, 279969, 279717,
    279464, 279212, 278960, 278709, 278457, 278206, 277955, 277704, 277454, 277204,
    276953, 276704, 276454, 276205, 275955, 275706, 275458, 275209, 274961, 274713,
    274465, 274217, 273970, 273723, 273476, 273229, 272983, 272737, 272491, 272245,
    271999, 271754, 271509, 271264, 271019, 270774, 270530, 270286, 270042, 269799,
    269555, 269312, 269069, 268826, 268584, 268342, 268100, 267858,
};

// Square-root pan law, 16384 at the centre: 16384 * sqrt(i / 128).
const uint16_t PAN_SQRT[257] = {
    0, 1448, 2048, 2508, 2896, 3238, 3547, 3831, 4096, 4344, 4579, 4803, 5017, 5221, 5418, 5609,
    5793, 5971, 6144, 6312, 6476, 6636, 6792, 6945, 7094, 7241, 7384, 7525, 7663, 7799, 7932, 8063,
    8192, 8319, 8444, 8567, 8689, 8809, 8927, 9044, 9159, 9273, 9385, 9496, 9606, 9715, 9822, 9928,
    10033, 10137, 10240, 10342, 10443, 10543, 10642, 10740, 10837, 10933, 11029, 11123, 11217, 11310, 11403, 11494,
    11585, 11675, 11765, 11854, 11942, 12029, 12116, 12202, 12288, 12373, 12457, 12541, 12625, 12708, 12790, 12871,
    12953, 13033, 13114, 13193, 13273, 13351, 13430, 13507, 13585, 13662, 13738, 13815, 13890, 13965, 14040, 14115,
    14189, 14263, 14336, 14409, 14482, 14554, 14626, 14697, 14768, 14839, 14910, 14980, 15050, 15119, 15188, 15257,
    15326, 15394, 15462, 15530, 15597, 15664, 15731, 15798, 15864, 15930, 15995, 16061, 16126, 16191, 16255, 16320,
    16384, 16448, 16512, 16575, 16638, 16701, 16764, 16826, 16888, 16950, 17012, 17073, 17135, 17196, 17257, 17317,
    17378, 17438, 17498, 17558, 17618, 17677, 17736, 17795, 17854, 17913, 17971, 18029, 18087, 18145, 18203, 18261,
    18318, 18375, 18432, 18489, 18545, 18602, 18658, 18714, 18770, 18826, 18882, 18937, 18992, 19048, 19102, 19157,
    19212, 19266, 19321, 19375, 19429, 19483, 19537, 19590, 19644, 19697, 19750, 19803, 19856, 19909, 19961, 20014,
    20066, 20118, 20170, 20222, 20274, 20326, 20377, 20429, 20480, 20531, 20582, 20633, 20684, 20734, 20785, 20835,
    20886, 20936, 20986, 21036, 21085, 21135, 21185, 21234, 21283, 21333, 21382, 21431, 21480, 21528, 21577, 21626,
    21674, 21722, 21771, 21819, 21867, 21915, 21962, 22010, 22058, 22105, 22153, 22200, 22247, 22294, 22341, 22388,
    22435, 22481, 22528, 22574, 22621, 22667, 22713, 22760, 22806, 22851, 22897, 22943, 22989, 23034, 23080, 23125,
    23170,
};

// Auto-vibrato sine, 256 steps, starts going down like FT2's.
const int8_t AUTOVIB_SINE[256] = {
    0, -2, -3, -5, -6, -8, -9, -11, -12, -14, -16, -17, -19, -20, -22, -23,
    -24, -26, -27, -29, -30, -32, -33, -34, -36, -37, -38, -39, -41, -42, -43, -44,
    -45, -46, -47, -48, -49, -50, -51, -52, -53, -54, -55, -56, -56, -57, -58, -59,
    -59, -60, -60, -61, -61, -62, -62, -62, -63, -63, -63, -64, -64, -64, -64, -64,
    -64, -64, -64, -64, -64, -64, -63, -63, -63, -62, -62, -62, -61, -61, -60, -60,
    -59, -59, -58, -57, -56, -56, -55, -54, -53, -52, -51, -50, -49, -48, -47, -46,
    -45, -44, -43, -42, -41, -39, -38, -37, -36, -34, -33, -32, -30, -29, -27, -26,
    -24, -23, -22, -20, -19, -17, -16, -14, -12, -11, -9, -8, -6, -5, -3, -2,
    0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 16, 17, 19, 20, 22, 23,
    24, 26, 27, 29, 30, 32, 33, 34, 36, 37, 38, 39, 41, 42, 43, 44,
    45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 56, 57, 58, 59,
    59, 60, 60, 61, 61, 62, 62, 62, 63, 63, 63, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 63, 63, 63, 62, 62, 62, 61, 61, 60, 60,
    59, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46,
    45, 44, 43, 42, 41, 39, 38, 37, 36, 34, 33, 32, 30, 29, 27, 26,
    24, 23, 22, 20, 19, 17, 16, 14, 12, 11, 9, 8, 6, 5, 3, 2,
};

// FT2's Amiga periods, 8 finetune steps a semitone from B-3 (OpenMPT's XMPeriodTable).
const uint16_t AMIGA_PERIODS[104] = {
    907, 900, 894, 887, 881, 875, 868, 862, 856, 850, 844, 838, 832, 826, 820, 814,
    808, 802, 796, 791, 785, 779, 774, 768, 762, 757, 752, 746, 741, 736, 730, 725,
    720, 715, 709, 704, 699, 694, 689, 684, 678, 675, 670, 665, 660, 655, 651, 646,
    640, 636, 632, 628, 623, 619, 614, 610, 604, 601, 597, 592, 588, 584, 580, 575,
    570, 567, 563, 559, 555, 551, 547, 543, 538, 535, 532, 528, 524, 520, 516, 513,
    508, 505, 502, 498, 494, 491, 487, 484, 480, 477, 474, 470, 467, 463, 460, 457,
    453, 450, 447, 443, 440, 437, 434, 431,
};

// FT2's vibrato and tremolo half sine, 32 steps.
const uint8_t VIB_TAB[32] = {
    0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24,
};

// An Amiga-mode period's rate in 1/16 Hz is this over the period (C-4 is 1712 at 8363 Hz).
const uint32_t AMIGA_FREQ16 = 8363u * 1712u * 16u;
const int NOTE_INDEXES = 1936; // FT2's note2Period table length
const int MAX_PERIOD = 31999;

inline uint16_t le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint8_t clampVol(int v) { return (uint8_t)(v < 0 ? 0 : v > 64 ? 64 : v); }

// OpenMPT's Amiga period for a 0-based note and finetune -128..127, in FT2's units.
inline uint32_t __not_in_flash_func(amigaPeriod)(int note, int finetune) {
    if (note < 0) note = 0;
    int rnote = (note % 12) << 3, roct = note / 12;
    int rfine = finetune / 16;
    int i = rnote + rfine + 8;
    i = i < 0 ? 0 : i > 103 ? 103 : i;
    uint32_t per1 = AMIGA_PERIODS[i];
    if (finetune < 0) { rfine--; finetune = -finetune; }
    else rfine++;
    i = rnote + rfine + 8;
    i = i < 0 ? 0 : i > 103 ? 103 : i;
    uint32_t per2 = AMIGA_PERIODS[i];
    rfine = finetune & 0x0F;
    return ((per1 * (16 - rfine) + per2 * rfine) << 1) >> roct;
}

} // namespace

// OpenMPT's tracker detection (Load_xm.cpp), reduced to what decides
// whether it plays the file as an old ModPlug one.
enum : uint16_t {
    VER_OLD_MODPLUG = 0x01, VER_NEW_MODPLUG = 0x02, VER_BIDI_FLAG = 0x04, VER_CONFIRMED = 0x10,
    VER_FT2_GENERIC = 0x20, VER_FT2_CLONE = 0x80, VER_PLAYERPRO = 0x100,
    VER_SAVED = 0x8000, // OpenMPT has set the save version, which nothing later clears
};

uint16_t XmPlayer::detectHeader(const uint8_t* hdr) {
    const char* tracker = (const char*)hdr + 38;
    const uint8_t* name = hdr + 17;
    uint16_t version = le16(hdr + 58), restartPos = le16(hdr + 66);
    uint16_t m;
    if (!memcmp(tracker, "FastTracker v2.00   ", 20) && le32(hdr + 60) == 276) {
        const uint8_t* nul = (const uint8_t*)memchr(name, 0, 20);
        if (version < 0x0104) {
            m = VER_FT2_GENERIC | VER_CONFIRMED;
        } else if (nul) {
            bool spaces = true;
            for (const uint8_t* q = nul + 1; q < name + 20; q++) spaces = spaces && *q == ' ';
            if (restartPos) m = VER_FT2_CLONE | VER_NEW_MODPLUG;
            else if (nul == name + 19) m = VER_FT2_CLONE | VER_NEW_MODPLUG | VER_PLAYERPRO;
            else if (spaces) m = VER_PLAYERPRO | VER_CONFIRMED;
            else m = VER_FT2_CLONE | VER_NEW_MODPLUG;
        } else {
            m = VER_FT2_GENERIC | VER_NEW_MODPLUG;
            if (!restartPos) m |= VER_PLAYERPRO;
        }
    } else if (!memcmp(tracker, "FastTracker v 2.00  ", 20)) {
        m = VER_OLD_MODPLUG;
    } else {
        return VER_CONFIRMED; // another tracker, played as FT2
    }
    if ((le16(hdr + 74) & 0x1000) && (m & VER_NEW_MODPLUG)) m = VER_FT2_CLONE | VER_NEW_MODPLUG | VER_CONFIRMED;
    return m;
}

void XmPlayer::detectInstrument(uint16_t& m, const uint8_t* ih, uint16_t numSamples, int& lastType, int64_t& lastShs,
                                bool& withSamples) {
    uint32_t size = le32(ih), shs = le32(ih + 29);
    const uint16_t sticky = m & VER_SAVED;
    if (m == VER_OLD_MODPLUG) {
        m = VER_CONFIRMED;
        if (size == 245 || size == 263) m |= VER_OLD_MODPLUG | VER_SAVED;
    } else if (numSamples == 0) {
        if (size == 263 && shs == 0 && (m & VER_NEW_MODPLUG)) m |= VER_CONFIRMED;
        else if ((m & (VER_FT2_CLONE | VER_FT2_GENERIC)) && size != 33) m = 0;
        if (size != 33) m &= ~VER_PLAYERPRO;
        else if (shs > 40 && (m & VER_PLAYERPRO)) {
            if (withSamples || (lastShs != -1 && shs != lastShs)) m = VER_PLAYERPRO | VER_CONFIRMED;
            lastShs = shs;
        }
    }
    if (lastType == -1) lastType = ih[26];
    else if (lastType != ih[26] && (m & VER_FT2_GENERIC)) m = (uint16_t)((m & ~VER_FT2_GENERIC) | VER_FT2_CLONE);
    if (numSamples > 0) {
        withSamples = true;
        if (ih[241] | ih[242] | ih[243] | ih[244] | ih[247]) m &= ~(VER_OLD_MODPLUG | VER_NEW_MODPLUG | VER_PLAYERPRO);
        if (size != 263 || ih[26] != 0) m &= ~VER_PLAYERPRO;
        if (!(m & VER_CONFIRMED) && (m & VER_PLAYERPRO)) {
            if ((!(ih[233] & 4) && ih[228] == 0xFF && ih[229] == 0xFF) || (!(ih[234] & 4) && ih[231] == 0xFF && ih[232] == 0xFF)) {
                m |= VER_CONFIRMED;
                m &= ~VER_NEW_MODPLUG;
            }
        }
    }
    m |= sticky;
}

void XmPlayer::detectSample(uint16_t& m, const uint8_t* sh, uint8_t& reservedOr, int& lastReserved) {
    uint8_t reserved = sh[17];
    reservedOr |= reserved;
    if (reserved != 0 && reserved != 0xAD) m &= ~(VER_OLD_MODPLUG | VER_NEW_MODPLUG);
    if (lastReserved == -1) lastReserved = reserved;
    else if (lastReserved != reserved) m &= ~VER_PLAYERPRO;
    if (sh[15] != 128) m &= ~VER_PLAYERPRO;
    if ((sh[13] & 0x0F) && sh[13] != 127) m &= ~VER_PLAYERPRO;
    if ((m & (VER_FT2_GENERIC | VER_FT2_CLONE)) && (m & (VER_NEW_MODPLUG | VER_PLAYERPRO)) && !(m & VER_CONFIRMED)) {
        bool named = reserved > 22;
        for (int i = 18 + reserved; i < 40 && !named; i++) named = sh[i] != ' ';
        if (named) m = (uint16_t)((m & ~VER_FT2_GENERIC) | VER_FT2_CLONE | VER_CONFIRMED);
    }
    if ((sh[14] & 3) == 3 && (m & VER_NEW_MODPLUG)) m |= VER_BIDI_FLAG;
}

// Reads the chunks after the samples. True for a file OpenMPT plays as old ModPlug.
bool XmPlayer::detectEnd(uint16_t m, const uint8_t* hdr, uint8_t reservedOr) {
    if (m & VER_SAVED) return true; // ModPlug 1.0 alpha or beta
    if (reservedOr == 0 && (m & VER_NEW_MODPLUG) && memchr(hdr + 17, 0, 20)) m |= VER_CONFIRMED;
    static const char* const tags[4] = {"text", "MIDI", "PNAM", "CNAM"};
    for (int i = 0; i < 4; i++) {
        uint8_t b[8];
        uint32_t at = _file.curPosition();
        if (_file.read(b, 8) != 8 || memcmp(b, tags[i], 4)) { _file.seekSet(at); continue; }
        m = (uint16_t)((m | VER_CONFIRMED) & ~VER_PLAYERPRO);
        _file.seekSet(at + 8 + le32(b + 4));
    }
    return (m & VER_CONFIRMED) && (m & (VER_BIDI_FLAG | VER_NEW_MODPLUG));
}

void XmPlayer::setMasterLevel(uint16_t m, bool oldModPlug, const uint8_t* hdr, int channels) {
    const char* tracker = (const char*)hdr + 38;
    uint8_t levels = MIX_COMPATIBLE;
    if (!memcmp(tracker, "OpenMPT ", 8)) {
        // Version digits are hex, 1.22.07.19 is 0x01220719.
        uint32_t ver = 0, part = 0;
        int parts = 0;
        for (int i = 8; i < 20; i++) {
            char c = tracker[i];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d >= 0) part = part * 16 + (uint32_t)d;
            else if (c == '.' && parts < 3) { ver = (ver << 8) | (part & 0xFF); part = 0; parts++; }
            else break;
        }
        ver = (ver << 8) | (part & 0xFF);
        while (++parts < 4) ver <<= 8;
        levels = ver < 0x01220719 ? MIX_COMPATIBLE : MIX_COMPATIBLE_FT2;
    } else if ((m & VER_FT2_GENERIC) || !memcmp(tracker, "Fasttracker II clone", 20)) {
        levels = MIX_COMPATIBLE_FT2;
    } else if (oldModPlug) {
        levels = MIX_ORIGINAL;
    } else if (!memcmp(tracker, "MilkyTracker", 12) && memcmp(tracker + 12, "        ", 8)) {
        levels = MIX_COMPATIBLE_FT2;
    }
    SongExtensions ext;
    readSongExtensions(0, 0, ext);
    if (ext.mixLevels >= 0 && ext.mixLevels <= MIX_COMPATIBLE_FT2) levels = (uint8_t)ext.mixLevels;
    uint32_t g = masterGainFor(levels, ext.preamp >= 0 ? (uint32_t)ext.preamp : 48, channels);
    _linearPan = levels != MIX_COMPATIBLE_FT2;
    // FT2's pan law is 1.0 at the side and 0.707 at the centre; PAN_SQRT
    // is that over sqrt(2).
    if (!_linearPan) g = (g * 46341 + 16384) >> 15;
    _masterGain = (uint16_t)(g > 4095 ? 4095 : g);
}

bool XmPlayer::load(const char* path) {
    close();
    _error[0] = '\0';

    if (!sdOpenPath(_file, path, O_RDONLY)) return loadFail("could not open file");
    if (!sdOpenPath(_patternFile, path, O_RDONLY)) return loadFail("could not open file");

    uint8_t hdr[80];
    if (_file.read(hdr, 80) != 80 || memcmp(hdr, "Extended Module: ", 17) != 0) return loadFail("unsupported format");
    // From offset 60: the header fields and the order table, then patterns.
    uint32_t headerSize = le32(hdr + 60);
    uint16_t songLenReal = le16(hdr + 64);
    uint16_t numChannelsReal = le16(hdr + 68);
    uint16_t numPatternsReal = le16(hdr + 70);
    uint16_t numInstrumentsReal = le16(hdr + 72);
    uint16_t flags = le16(hdr + 74);
    uint16_t defaultTempo = le16(hdr + 76);
    uint16_t defaultBpm = le16(hdr + 78);

    // Sanity bounds against a corrupt file claiming absurd counts.
    if (headerSize < 20 || numChannelsReal < 1 || numChannelsReal > 128 || numPatternsReal > 4096 ||
        numInstrumentsReal > 4096)
        return loadFail("invalid XM file");

    _linearFreqTable = (flags & 0x01) != 0;
    // Which tracker saved the file, OpenMPT's way. It plays files from old
    // ModPlug versions without FT2's quirks; of those this player only
    // copies the finetune precision.
    uint16_t madeWith = detectHeader(hdr);
    int lastInstrType = -1, lastSampleReserved = -1;
    int64_t lastSampleHeaderSize = -1;
    uint8_t sampleReserved = 0;
    bool instrumentWithSamples = false;
    _fileNumChannels = numChannelsReal;
    _numChannels = (numChannelsReal < (uint16_t)MAX_CHANNELS) ? (int)numChannelsReal : MAX_CHANNELS;

    // The order table is up to 256 bytes, fewer when the header is shorter.
    uint32_t orderBytes = headerSize - 20 < (uint32_t)MAX_ORDERS ? headerSize - 20 : (uint32_t)MAX_ORDERS;
    memset(_orderTable, 0, sizeof(_orderTable));
    if (orderBytes && _file.read(_orderTable, orderBytes) != (int)orderBytes) return loadFail("invalid XM file");
    _songLength = songLenReal < orderBytes ? (int)songLenReal : (int)orderBytes;

    if (!_file.seekSet(60 + headerSize)) return loadFail("invalid XM file");
    uint32_t fileSize = (uint32_t)_file.fileSize();

    // Patterns the walk doesn't reach stay empty 64-row patterns.
    for (int i = 0; i < MAX_PATTERNS; i++) {
        _patternSizes[i] = 0;
        _patternRowCounts[i] = 64;
        _patternOffsets[i] = 0;
    }
    for (int i = 0; i < (int)numPatternsReal; i++) {
        uint8_t ph[9];
        if (_file.read(ph, 9) != 9) break; // truncated: what was found stands
        uint32_t patHeaderLen = le32(ph);
        uint16_t numRows = le16(ph + 5);
        uint16_t packedSize = le16(ph + 7);
        if (patHeaderLen > 9) _file.seekCur(patHeaderLen - 9);
        uint32_t cellDataStart = _file.curPosition();
        if (i < MAX_PATTERNS) {
            _patternOffsets[i] = cellDataStart;
            _patternRowCounts[i] = (numRows > 0 && numRows <= MAX_ROWS) ? numRows : 64;
            uint32_t avail = fileSize > cellDataStart ? fileSize - cellDataStart : 0;
            _patternSizes[i] = (uint16_t)(packedSize < avail ? packedSize : avail);
        }
        if (!_file.seekSet(cellDataStart + packedSize)) break;
    }

    // Instruments, each followed by its sample headers and then their data.
    for (int i = 0; i < MAX_INSTRUMENTS; i++) _instruments[i] = XmInstrument();
    _numInstruments = 0;
    _numSamplesTotal = 0;
    _numAdpcm = 0;
    for (int i = 0; i < (int)numInstrumentsReal; i++) {
        uint32_t instStart = _file.curPosition();
        // The voice slots are free until play, so the instrument header and
        // the sample headers go there to keep load()'s stack small.
        const uint32_t IH_BYTES = 263;
        uint8_t* ih = (uint8_t*)&_chunks[1][0];
        memset(ih, 0, IH_BYTES);
        if (_file.read(ih, 29) != 29) break;
        uint32_t isize = le32(ih);
        uint16_t numSamplesReal = le16(ih + 27);
        // The declared size wins; fields past a short header read as zero.
        uint32_t hdrLen = isize < 29 ? 29 : isize;
        if (numSamplesReal > 0) {
            uint32_t extra = (hdrLen > IH_BYTES ? IH_BYTES : hdrLen) - 29;
            if (extra && _file.read(ih + 29, extra) != (int)extra) break;
        }
        if (numSamplesReal > 64) return loadFail("invalid XM file");
        if (!_file.seekSet(instStart + hdrLen)) break;
        uint32_t sampleHeaderSize = le32(ih + 29);
        detectInstrument(madeWith, ih, numSamplesReal, lastInstrType, lastSampleHeaderSize, instrumentWithSamples);
        if (sampleHeaderSize < 18 || sampleHeaderSize > 1024) sampleHeaderSize = 40;

        XmInstrument* inst = i < MAX_INSTRUMENTS ? &_instruments[i] : nullptr;
        if (inst && numSamplesReal > 0) {
            for (int n = 0; n < 96; n++) inst->keymap[n >> 1] |= (uint8_t)((ih[33 + n] & 0x0F) << ((n & 1) * 4));
            XmEnvelope* envs[2] = {&inst->volEnv, &inst->panEnv};
            for (int e = 0; e < 2; e++) {
                XmEnvelope& env = *envs[e];
                const uint8_t* pts = ih + 129 + e * 48;
                for (int p = 0; p < MAX_ENV_POINTS; p++) {
                    env.pointX[p] = le16(pts + p * 4);
                    uint16_t y = le16(pts + p * 4 + 2);
                    env.pointY[p] = (uint8_t)(y > 64 ? 64 : y);
                }
                uint8_t num = ih[225 + e];
                env.numPoints = num <= MAX_ENV_POINTS ? num : (uint8_t)MAX_ENV_POINTS;
                env.sustainPoint = ih[227 + e * 3];
                env.loopStartPoint = ih[228 + e * 3];
                env.loopEndPoint = ih[229 + e * 3];
                env.flags = ih[233 + e];
                if (env.loopStartPoint >= env.numPoints) env.loopStartPoint = 0;
            }
            inst->vibType = ih[235];
            inst->vibSweep = ih[236];
            inst->vibDepth = ih[237] > 15 ? 15 : ih[237];
            inst->vibRate = ih[238];
            inst->volFadeout = le16(ih + 239);
        }
        if (inst) inst->firstSampleIndex = (uint8_t)_numSamplesTotal;

        struct TempSampleInfo {
            uint32_t lengthBytes, loopStartBytes, loopLengthBytes;
            uint8_t volume, flags, panning, reserved;
            int8_t finetune, relativeNote;
        };
        static_assert(64 * sizeof(TempSampleInfo) <= sizeof(_chunks[0]), "sample headers fit a voice slot");
        TempSampleInfo* temp = reinterpret_cast<TempSampleInfo*>(&_chunks[0][0]);
        int numSamplesParsed = (int)numSamplesReal;
        for (int k = 0; k < numSamplesParsed; k++) {
            uint32_t shStart = _file.curPosition();
            uint8_t sb[40];
            memset(sb + 18, ' ', 22);
            if (_file.read(sb, 18) != 18) { numSamplesParsed = k; break; }
            _file.read(sb + 18, 22); // the name, only for detection
            detectSample(madeWith, sb, sampleReserved, lastSampleReserved);
            temp[k].lengthBytes = le32(sb);
            temp[k].loopStartBytes = le32(sb + 4);
            temp[k].loopLengthBytes = le32(sb + 8);
            temp[k].volume = sb[12];
            temp[k].finetune = (int8_t)sb[13];
            temp[k].flags = sb[14];
            temp[k].panning = sb[15];
            temp[k].relativeNote = (int8_t)sb[16];
            temp[k].reserved = sb[17];
            _file.seekSet(shStart + sampleHeaderSize);
        }

        for (int k = 0; k < numSamplesParsed; k++) {
            uint32_t pcmStart = _file.curPosition();
            const TempSampleInfo& t = temp[k];
            bool is16Bit = (t.flags & 0x10) != 0;
            // ModPlug's ADPCM: 16 deltas, then a nibble a frame, low first.
            bool adpcm = t.reserved == 0xAD && !is16Bit;
            uint32_t dataBytes = adpcm ? 16 + (t.lengthBytes + 1) / 2 : t.lengthBytes;
            bool storeIt = inst && inst->numSamplesUsed < MAX_SAMPLES_PER_INSTRUMENT && _numSamplesTotal < MAX_SAMPLES;
            if (storeIt) {
                XmSample& s = _samples[_numSamplesTotal];
                s = XmSample();
                uint32_t bpf = is16Bit ? 2 : 1;
                s.format = adpcm ? FMT_ADPCM : is16Bit ? FMT_16BIT : FMT_8BIT;
                s.fileOffset = pcmStart + (adpcm ? 16 : 0);
                s.length = t.lengthBytes / bpf;
                // Data cut short by the end of the file.
                uint32_t avail = fileSize > s.fileOffset ? fileSize - s.fileOffset : 0;
                uint32_t availFrames = adpcm ? avail * 2 : avail / bpf;
                if (s.length > availFrames) s.length = availFrames;
                if (adpcm) {
                    if (_numAdpcm < MAX_ADPCM && _file.read(_adpcmTables[_numAdpcm], 16) == 16) s.adpcmSlot = (uint8_t)_numAdpcm++;
                    else s.length = 0;
                }
                uint32_t ls = t.loopStartBytes / bpf, ll = t.loopLengthBytes / bpf;
                uint8_t loopType = t.flags & 0x03;
                if (loopType && ll > 0 && ls < s.length) {
                    if (ll > s.length - ls) ll = s.length - ls;
                    s.looping = true;
                    s.pingpong = loopType >= 2;
                    s.loopStart = ls;
                    s.realLoopEnd = s.loopEnd = ls + ll;
                    int32_t acc = 0;
                    decodeForward(s, 0, ls, acc, nullptr, 0);
                    s.accumAtLoopStart = (int16_t)acc;
                    decodeForward(s, ls, ll, acc, nullptr, 0);
                    s.accumAtLoopEnd = (int16_t)acc;
                    if (s.pingpong) {
                        s.loopEnd = s.realLoopEnd + ll;
                        if (s.length < s.loopEnd) s.length = s.loopEnd;
                    }
                }
                s.volume = t.volume > 64 ? 64 : t.volume;
                s.finetune = t.finetune;
                s.panning = t.panning;
                s.relativeNoteNumber = t.relativeNote;
                inst->numSamplesUsed++;
                _numSamplesTotal++;
            }
            if (!_file.seekSet(pcmStart + dataBytes)) break;
        }

        if (inst && numSamplesReal > 0) _numInstruments++;
    }

    _exactFinetune = detectEnd(madeWith, hdr, sampleReserved);
    setMasterLevel(madeWith, _exactFinetune, hdr, numChannelsReal);

    for (int ch = 0; ch < MAX_CHANNELS; ch++) _channels[ch] = XmChannel();
    attachVoices(&_channels[0], sizeof(XmChannel), _numChannels, &_chunks[0][0], CHUNK_FRAMES);
    useSampleFile(_file);
    attachTails(_tails, (int)(sizeof(_tails) / sizeof(_tails[0])), _tailPool, TAIL_POOL_FRAMES);
    for (int i = 0; i < NUM_SEEK_HINTS; i++) _seekHints[i] = SeekHint{0, 0, 0xFF};
    _nextSeekHint = 0;

    _globalVolume = 64;
    _rowDelay = _pendingRowDelay = 0;
    _openPatternNum = -1;
    _openPatternEnd = 0;
    _rowOffsetsValid = 0;
    uint16_t bpm = defaultBpm < 32 ? (defaultBpm ? 32 : 125) : defaultBpm > 999 ? 999 : defaultBpm;
    resetSequencer(defaultTempo > 0 && defaultTempo < 256 ? (uint8_t)defaultTempo : 6, bpm);
    return true;
}

// FT2's note2Period: index is (note - 1) * 16 plus the finetune slot,
// (finetune >> 3) + 16. Linear periods are 64 a semitone.
uint16_t __not_in_flash_func(XmPlayer::note2Period)(int index) const {
    if (index < 0) index = 0;
    if (index >= NOTE_INDEXES) index = NOTE_INDEXES - 1;
    if (_linearFreqTable) return (uint16_t)((NOTE_INDEXES - index) * 4);
    uint32_t p = amigaPeriod((index >> 4) - 1, (index & 15) << 3);
    return (uint16_t)(p > 65535 ? 65535 : p);
}

// Period of a 0-based note (relative note included), 0 when out of range.
// Old ModPlug files keep the finetune's low 3 bits, as OpenMPT plays them.
uint16_t __not_in_flash_func(XmPlayer::notePeriod)(int note, int fineTune) const {
    int idx = (note << 4) + ((fineTune >> 3) + 16);
    if (idx < 0 || idx >= NOTE_INDEXES) return 0;
    if (!_exactFinetune) return note2Period(idx);
    if (_linearFreqTable) {
        int p = 7680 - note * 64 - fineTune / 2;
        return (uint16_t)(p < 1 ? 1 : p);
    }
    uint32_t p = amigaPeriod(note, fineTune);
    return (uint16_t)(p > 65535 ? 65535 : p);
}

// FT2's relocateTon: the note nearest `period` at the channel's finetune,
// plus arpNote semitones. It only searches 8 octaves, as FT2 does.
uint16_t __not_in_flash_func(XmPlayer::relocateTon)(uint16_t period, int arpNote, const XmChannel& c) const {
    const int fineTune = (c.fineTune >> 3) + 16;
    int hi = 8 * 12 * 16, lo = 0;
    for (int i = 0; i < 8; i++) {
        int tmp = (((lo + hi) >> 1) & ~15) + fineTune;
        int lookUp = tmp - 8;
        if (lookUp < 0) lookUp = 0;
        if (period >= note2Period(lookUp)) hi = (tmp - fineTune) & ~15;
        else lo = (tmp - fineTune) & ~15;
    }
    int tmp = lo + fineTune + (arpNote << 4);
    if (tmp >= (8 * 12 * 16 + 15) - 1) tmp = (8 * 12 * 16 + 16) - 1;
    return note2Period(tmp);
}

// Step in 1/2^23 frames for a period. Rates are in 1/16 Hz as OpenMPT
// works them out: linear from FT2's table, Amiga from the 8363 Hz C-4.
uint32_t __not_in_flash_func(XmPlayer::periodToStep23)(uint32_t period) const {
    if (period == 0) return 0;
    uint32_t f16;
    if (_linearFreqTable) {
        uint32_t oct = period / 768;
        f16 = oct >= 24 ? 0 : (LINEAR_FREQ[period % 768] << 4) >> oct;
    } else {
        f16 = AMIGA_FREQ16 / period;
    }
    if (f16 >= (1u << 23)) return MAX_STEP16 << 7;
    // f16 * 2^19 / SAMPLE_RATE in two 32-bit steps.
    uint32_t a = f16 << 9;
    uint32_t q = a / SAMPLE_RATE, r = a % SAMPLE_RATE;
    uint32_t step23 = (q << 10) + (r << 10) / SAMPLE_RATE;
    return step23 > (MAX_STEP16 << 7) ? MAX_STEP16 << 7 : step23;
}

// Decodes frames [first, first + count) of s into out (when not null),
// skipping the first `skip`. accum carries the value of the frame before.
// Returns the frames decoded, fewer on a short read.
uint32_t __not_in_flash_func(XmPlayer::decodeForward)(const XmSample& s, uint32_t first, uint32_t count, int32_t& accum,
                                                     int16_t* out, uint32_t skip) {
    const int8_t* tab = _adpcmTables[s.adpcmSlot];
    int32_t a = accum;
    uint32_t done = 0;
    while (done < count) {
        uint32_t k = first + done, left = count - done;
        uint32_t at, want;
        if (s.format == FMT_16BIT) { at = s.fileOffset + k * 2; want = left * 2; }
        else if (s.format == FMT_ADPCM) { at = s.fileOffset + (k >> 1); want = ((k + left - 1) >> 1) - (k >> 1) + 1; }
        else { at = s.fileOffset + k; want = left; }
        uint32_t span = rawSpan(at);
        if (want > span) want = span & ~1u; // even, so 16-bit frames stay whole
        int n = 0;
        const uint8_t* raw = readSampleBytes(_file, at, want, n);
        if (n <= 0) break;
        uint32_t frames;
        if (s.format == FMT_16BIT) frames = (uint32_t)n / 2;
        else if (s.format == FMT_ADPCM) frames = (uint32_t)n * 2 - (k & 1);
        else frames = (uint32_t)n;
        if (frames > left) frames = left;
        // Frames before `skip` are decoded and dropped.
        uint32_t drop = done < skip ? skip - done : 0;
        if (drop > frames || !out) drop = frames;
        int16_t* o = out ? out + (done + drop - skip) : nullptr;
        if (s.format == FMT_16BIT) {
            uint32_t i = 0;
            for (; i < drop; i++) a = (int16_t)(a + (int16_t)le16(raw + i * 2));
            for (; i < frames; i++) { a = (int16_t)(a + (int16_t)le16(raw + i * 2)); *o++ = (int16_t)a; }
        } else if (s.format == FMT_ADPCM) {
            uint32_t f = k & 1, i = 0;
            for (; i < drop; i++, f++) { uint8_t b = raw[f >> 1]; a = (int8_t)(a + tab[(f & 1) ? (b >> 4) : (b & 15)]); }
            for (; i < frames; i++, f++) {
                uint8_t b = raw[f >> 1];
                a = (int8_t)(a + tab[(f & 1) ? (b >> 4) : (b & 15)]);
                *o++ = (int16_t)(a * 256);
            }
        } else {
            uint32_t i = 0;
            for (; i < drop; i++) a = (int8_t)(a + (int8_t)raw[i]);
            for (; i < frames; i++) { a = (int8_t)(a + (int8_t)raw[i]); *o++ = (int16_t)(a * 256); }
        }
        done += frames;
        if ((uint32_t)n < want) break;
    }
    accum = a;
    return done;
}

// Fills out with frames high, high - 1 .. low, stepping back through the
// deltas. With haveTop, v is frame high's value, else frame high + 1's.
bool __not_in_flash_func(XmPlayer::decodeBackward)(const XmSample& s, uint32_t low, uint32_t high, int32_t v, bool haveTop,
                                                   int16_t* out) {
    const int8_t* tab = _adpcmTables[s.adpcmSlot];
    const int shift = s.format == FMT_16BIT ? 0 : 8;
    uint32_t o = 0;
    if (haveTop) out[o++] = (int16_t)(v << shift);
    // Frame f's delta takes frame f's value back to frame f - 1's.
    uint32_t f = haveTop ? high : high + 1;
    const uint32_t lowest = low + 1;
    const uint32_t most = s.format == FMT_16BIT ? 512 : s.format == FMT_ADPCM ? 2000 : 1024; // 1 KB or less a read
    while (f >= lowest && f > 0) {
        uint32_t pieceLo = f - lowest + 1 > most ? f - most + 1 : lowest;
        uint32_t at, len;
        if (s.format == FMT_16BIT) { at = s.fileOffset + pieceLo * 2; len = (f - pieceLo + 1) * 2; }
        else if (s.format == FMT_ADPCM) { at = s.fileOffset + (pieceLo >> 1); len = (f >> 1) - (pieceLo >> 1) + 1; }
        else { at = s.fileOffset + pieceLo; len = f - pieceLo + 1; }
        int n = 0;
        const uint8_t* raw = readSampleBytes(_file, at, len, n);
        if ((uint32_t)n < len) return false;
        if (s.format == FMT_16BIT) {
            for (uint32_t g = f + 1; g-- > pieceLo;) { v = (int16_t)(v - (int16_t)le16(raw + (g - pieceLo) * 2)); out[o++] = (int16_t)v; }
        } else if (s.format == FMT_ADPCM) {
            uint32_t base = pieceLo & ~1u;
            for (uint32_t g = f + 1; g-- > pieceLo;) {
                uint8_t b = raw[(g - base) >> 1];
                v = (int8_t)(v - tab[(g & 1) ? (b >> 4) : (b & 15)]);
                out[o++] = (int16_t)(v * 256);
            }
        } else {
            for (uint32_t g = f + 1; g-- > pieceLo;) { v = (int8_t)(v - (int8_t)raw[g - pieceLo]); out[o++] = (int16_t)(v * 256); }
        }
        f = pieceLo - 1;
    }
    return true;
}

// Value of frame index - 1, decoded from the nearest known point at or
// before index: the start, the loop start, a seek hint or the end of a
// forward chunk some channel holds. Long decodes leave a seek hint.
int32_t __not_in_flash_func(XmPlayer::accumAt)(const XmSample& s, uint32_t index) {
    uint32_t anchor = 0;
    int32_t a = 0;
    if (s.looping && s.loopStart <= index) { anchor = s.loopStart; a = s.accumAtLoopStart; }
    uint8_t si = (uint8_t)(&s - _samples);
    for (int i = 0; i < NUM_SEEK_HINTS; i++) {
        const SeekHint& h = _seekHints[i];
        if (h.sample == si && h.index <= index && h.index > anchor) { anchor = h.index; a = h.accum; }
    }
    for (int ch = 0; ch < _numChannels; ch++) {
        const XmChannel& c = _channels[ch];
        if (c.chunkSmp != &s || !c.chunkLen || (s.pingpong && c.chunkStart >= s.realLoopEnd)) continue;
        uint32_t end = c.chunkStart + c.chunkLen;
        if (end <= index && end > anchor) { anchor = end; a = c.chunkAccum; }
    }
    if (index > anchor) {
        uint32_t got = decodeForward(s, anchor, index - anchor, a, nullptr, 0);
        if (got == index - anchor && got >= 2048) { // a short read leaves no hint
            _seekHints[_nextSeekHint] = SeekHint{index, (int16_t)a, si};
            _nextSeekHint = (uint8_t)((_nextSeekHint + 1) % NUM_SEEK_HINTS);
        }
    }
    return a;
}

// Refills v's cache from `index`. Forward reads carry the accumulator on;
// frames past a ping-pong loop's end are decoded backwards from the turn.
int16_t __not_in_flash_func(XmPlayer::refillChunk)(TrackerVoice& v, uint32_t index) {
    const XmSample& s = static_cast<const XmSample&>(*v.smp);
    const uint32_t turn = s.realLoopEnd;
    const bool loops = v.looping && s.loopEnd > s.loopStart;
    const bool mirror = loops && s.pingpong && index >= turn;
    uint32_t limit = mirror ? s.loopEnd : (loops && index < turn) ? turn : s.length;
    if (index >= limit) { v.chunkLen = 0; return 0; }
    uint32_t n = limit - index;
    if (n > (uint32_t)CHUNK_FRAMES) n = CHUNK_FRAMES;
    if (shareChunk(v, index, n, n, true)) return v.chunkBuf[0];
    const bool cont = v.chunkSmp == v.smp && v.chunkLen && index == v.chunkStart + v.chunkLen;
    const bool prevMirror = s.pingpong && v.chunkStart >= turn;

    if (mirror) {
        uint32_t high = 2 * turn - 1 - index, low = high + 1 - n;
        bool ok;
        if (cont && prevMirror) ok = decodeBackward(s, low, high, v.chunkAccum, false, v.chunkBuf);
        else if (index == turn) ok = decodeBackward(s, low, high, s.accumAtLoopEnd, true, v.chunkBuf);
        else ok = decodeBackward(s, low, high, accumAt(s, high + 1), true, v.chunkBuf);
        if (!ok) { v.chunkLen = 0; return 0; }
        v.chunkAccum = s.format == FMT_16BIT ? v.chunkBuf[n - 1] : v.chunkBuf[n - 1] >> 8;
    } else {
        int32_t a;
        uint32_t from = index, skip = 0;
        if (cont && !prevMirror) a = v.chunkAccum;
        else if (index == 0) a = 0;
        else if (loops && index == s.loopStart) a = s.accumAtLoopStart;
        else {
            // A loop wrap usually lands a few frames past loopStart: decode
            // that gap in the same read. Bigger gaps (9xx) catch up first.
            uint32_t anchor = (loops && index >= s.loopStart) ? s.loopStart : 0;
            if (index - anchor <= 64) {
                from = anchor;
                skip = index - anchor;
                a = anchor ? s.accumAtLoopStart : 0;
            } else {
                a = accumAt(s, index);
            }
        }
        uint32_t got = decodeForward(s, from, skip + n, a, v.chunkBuf, skip);
        if (got <= skip) { v.chunkLen = 0; return 0; }
        n = got - skip;
        v.chunkAccum = a;
    }
    v.chunkStart = index;
    v.chunkLen = n;
    v.chunkSmp = v.smp;
    return v.chunkBuf[0];
}

void XmPlayer::openPatternForScan(int num) {
    _openPatternNum = num;
    _rowOffsetsValid = 0;
    if (num >= 0 && num < MAX_PATTERNS) {
        _scanCursor = _patternOffsets[num];
        _openPatternEnd = _scanCursor + _patternSizes[num];
        _openPatternRowCount = _patternRowCounts[num];
    } else {
        _scanCursor = 0;
        _openPatternEnd = 0;
        _openPatternRowCount = 64;
    }
}

void XmPlayer::advanceToRow(int targetRow) {
    if (targetRow < 0 || targetRow >= MAX_ROWS) return;
    if (targetRow < _rowOffsetsValid) {
        _patternFile.seekSet(_rowOffsets[targetRow]);
        decodeRow(true);
        return;
    }
    while (_rowOffsetsValid <= targetRow && _rowOffsetsValid < _openPatternRowCount) {
        _rowOffsets[_rowOffsetsValid] = _scanCursor;
        _patternFile.seekSet(_scanCursor);
        decodeRow(_rowOffsetsValid == targetRow); // earlier rows are only parsed, for their length
        _scanCursor = _patternFile.curPosition();
        _rowOffsetsValid++;
    }
}

// Reads one row: _fileNumChannels packed cells back to back. A blank or
// truncated pattern gives empty cells, so every channel still sees the row.
// Nothing is read past the pattern's packed size.
void XmPlayer::decodeRow(bool apply) {
    uint32_t pos = _patternFile.curPosition();
    bool ok = pos < _openPatternEnd;
    for (int ch = 0; ch < _fileNumChannels; ch++) {
        uint8_t b[5] = {0, 0, 0, 0, 0}; // note, instrument, volume, effect, param
        if (ok) {
            uint8_t flag;
            if (pos >= _openPatternEnd || _patternFile.read(&flag, 1) != 1) {
                ok = false;
            } else if (flag & 0x80) {
                uint8_t got[5];
                int need = 0;
                for (int k = 0; k < 5; k++) need += (flag >> k) & 1;
                if (pos + 1 + need > _openPatternEnd || (need && _patternFile.read(got, need) != need)) ok = false;
                for (int k = 0, j = 0; ok && k < 5; k++) if (flag & (1 << k)) b[k] = got[j++];
                pos += 1 + need;
            } else {
                // Unpacked: the flag byte is the note and the other four follow.
                b[0] = flag;
                if (pos + 5 > _openPatternEnd || _patternFile.read(b + 1, 4) != 4) ok = false;
                pos += 5;
            }
            if (!ok) memset(b, 0, sizeof(b));
        }
        if (apply && ch < _numChannels) {
            if (b[0] > 97) b[0] = 0;
            if (b[3] > 35) b[3] = b[4] = 0;
            UiTap::cell(ch, b[0] == 97 ? UiTap::NOTE_OFF : b[0], b[1]);
            rowCell(ch, b[0], b[1], b[2], b[3], b[4]);
        }
    }
}

// ---------------------------------------------------------------- replayer
// After FT2's replayer (getNewNote, handleEffects_TickZero, doEffects and
// fixaEnvelopeVibrato), with OpenMPT's choices where the two differ.

void __not_in_flash_func(XmPlayer::resetVolumes)(XmChannel& c) {
    c.realVol = c.outVol = c.oldVol;
    c.outPan = c.oldPan;
}

void __not_in_flash_func(XmPlayer::triggerInstrument)(XmChannel& c) {
    if (!(c.waveCtrl & 0x04)) c.vibPos = 0;
    if (!(c.waveCtrl & 0x40)) c.tremPos = 0;
    c.retrigCnt = 0;
    c.tremorPos = 0;
    c.sustainActive = true;
    c.fadeOutAmp = 32768;
    c.fadeOutSpeed = 0;
    const XmInstrument* in = instrumentOf(c.envInst);
    if (!in) return;
    // Start from the first point's value, as OpenMPT, even when it isn't at tick 0.
    if (in->volEnv.enabled()) c.volEnv = XmEnvState{65535, (int16_t)(in->volEnv.pointY[0] << 8), 0, 0};
    if (in->panEnv.enabled()) c.panEnv = XmEnvState{65535, (int16_t)(in->panEnv.pointY[0] << 8), 0, 0};
    c.fadeOutSpeed = in->volFadeout;
    if (in->vibDepth) {
        c.eVibPos = 0;
        if (in->vibSweep) { c.eVibAmp = 0; c.eVibSweep = (uint16_t)((in->vibDepth << 8) / in->vibSweep); }
        else { c.eVibAmp = (uint16_t)(in->vibDepth << 8); c.eVibSweep = 0; }
    }
}

// Releases sustain. Without a volume envelope the volume drops to 0 (the
// sample keeps playing, so a later volume brings it back).
void __not_in_flash_func(XmPlayer::keyOff)(XmChannel& c) {
    c.sustainActive = false;
    const XmInstrument* in = instrumentOf(c.envInst);
    if (in && in->volEnv.enabled()) {
        uint16_t x = in->volEnv.pointX[c.volEnv.pos];
        if (c.volEnv.cnt >= x) c.volEnv.cnt = (uint16_t)(x - 1);
    } else {
        c.realVol = c.outVol = 0;
    }
    if (in && in->panEnv.enabled()) {
        uint16_t x = in->panEnv.pointX[c.panEnv.pos];
        if (c.panEnv.cnt >= x) c.panEnv.cnt = (uint16_t)(x - 1);
    }
}

// Plays note ton (1-96, 0 == the last note again) on the channel's
// instrument, from 9xx's offset when effTyp is 9. Leaves the volume alone.
void __not_in_flash_func(XmPlayer::triggerNote)(XmChannel& c, uint8_t ton, uint8_t effTyp, uint8_t eff) {
    if (ton == 97) { keyOff(c); return; }
    if (ton == 0) {
        ton = c.tonNr;
        if (ton == 0) return;
    }
    c.tonNr = ton;
    c.envInst = c.instrNum;
    const XmInstrument* in = instrumentOf(c.instrNum);
    const XmSample* s = nullptr;
    c.sampleIndex = 0xFF;
    if (in && in->numSamplesUsed) {
        uint8_t rel = in->sampleFor(ton - 1);
        if (rel < in->numSamplesUsed) {
            c.sampleIndex = (uint8_t)(in->firstSampleIndex + rel);
            s = &_samples[c.sampleIndex];
        }
    }
    c.relTonNr = s ? s->relativeNoteNumber : 0;
    int t = (int)ton + c.relTonNr;
    if (t >= 12 * 10) return;
    c.oldVol = s ? s->volume : 0;
    c.oldPan = s ? s->panning : 128;
    if (effTyp == 0x0E && (eff & 0xF0) == 0x50) c.fineTune = (int8_t)(((eff & 0x0F) << 4) - 128); // E5x
    else c.fineTune = s ? s->finetune : 0;
    if (t >= 1) {
        uint16_t per = notePeriod(t - 1, c.fineTune);
        if (per) c.outPeriod = c.realPeriod = per;
    }
    uint32_t start = 0;
    if (effTyp == 9) {
        if (eff) c.smpOffset = eff;
        start = (uint32_t)c.smpOffset << 8;
    }
    // An offset past the end (or the loop end) doesn't play, as FT2.
    if (!s || s->length == 0 || start >= (s->looping ? s->realLoopEnd : s->length)) {
        c.smp = nullptr;
        c.havePair = false;
        return;
    }
    c.smp = s;
    c.looping = s->looping;
    startVoiceAt(c, start);
}

void XmPlayer::preparePortamento(XmChannel& c, uint8_t note, uint8_t inst) {
    if (note == 97) {
        keyOff(c);
    } else if (note > 0) {
        // The target uses the playing sample's relative note and finetune.
        uint16_t per = notePeriod((note - 1) + c.relTonNr, c.fineTune);
        if (per) {
            c.wantPeriod = per;
            c.portaDir = c.wantPeriod == c.realPeriod ? 0 : c.wantPeriod > c.realPeriod ? 1 : 2;
        }
    }
    if (inst) {
        resetVolumes(c);
        if (note != 97) triggerInstrument(c);
    }
}

// Tick 0 of a row: the cell's note, instrument and effects.
void XmPlayer::rowCell(int ch, uint8_t note, uint8_t inst, uint8_t vol, uint8_t effTyp, uint8_t eff) {
    XmChannel& c = _channels[ch];
    c.volCol = vol;
    // Leaving arpeggio or vibrato goes back to the base pitch; vibrato
    // that carries on keeps its offset through tick 0.
    if (c.effTyp == 0) {
        if (c.eff != 0) c.outPeriod = c.realPeriod;
    } else if ((c.effTyp == 4 || c.effTyp == 6) && effTyp != 4 && effTyp != 6) {
        c.outPeriod = c.realPeriod;
    }
    c.effTyp = effTyp;
    c.eff = eff;
    c.rowNote = note;
    c.rowInst = inst;
    if (inst > 0) {
        if (inst <= MAX_INSTRUMENTS) c.instrNum = inst;
        else inst = 0;
    }

    bool checkEfx = true;
    if (effTyp == 0x0E) {
        if (eff >= 0xD1 && eff <= 0xDF) return; // EDx: the whole cell waits for tick x
        if (eff == 0x90) checkEfx = false;      // E90 retriggers on tick 0
    }
    if (checkEfx) {
        if ((vol & 0xF0) == 0xF0) {
            if (vol & 0x0F) c.portaSpeed = (uint16_t)((vol & 0x0F) << 6);
            preparePortamento(c, note, inst);
            tickZeroEffects(ch);
            return;
        }
        if (effTyp == 3 || effTyp == 5) {
            if (effTyp != 5 && eff != 0) c.portaSpeed = (uint16_t)(eff << 2);
            preparePortamento(c, note, inst);
            tickZeroEffects(ch);
            return;
        }
        if (effTyp == 0x14 && eff == 0) { // K00: key off, the note isn't played
            keyOff(c);
            if (inst) resetVolumes(c);
            tickZeroEffects(ch);
            return;
        }
        if (note == 0) {
            if (inst) { resetVolumes(c); triggerInstrument(c); }
            tickZeroEffects(ch);
            return;
        }
    }
    if (note == 97) keyOff(c);
    else triggerNote(c, note, effTyp, eff);
    if (inst) { resetVolumes(c); triggerInstrument(c); }
    tickZeroEffects(ch);
}

void XmPlayer::envSetPos(const XmEnvelope& e, XmEnvState& st, uint8_t param) {
    st.cnt = (uint16_t)(param - 1);
    int point = 0, tick = param;
    bool update = true;
    if (e.numPoints > 1) {
        point++;
        for (int i = 0; i < e.numPoints - 1; i++) {
            if (tick < e.pointX[point]) {
                point--;
                tick -= e.pointX[point];
                if (tick == 0) { update = false; break; }
                int dx = e.pointX[point + 1] - e.pointX[point];
                if (dx <= 0) break;
                int y0 = e.pointY[point], y1 = e.pointY[point + 1];
                st.ip = (int16_t)(((y1 - y0) << 8) / dx);
                st.amp = (int16_t)(st.ip * (tick - 1) + (y0 << 8));
                point++;
                update = false;
                break;
            }
            point++;
        }
        if (update) point--;
    }
    if (update) {
        st.ip = 0;
        st.amp = (int16_t)(e.pointY[point] << 8);
    }
    if (point >= e.numPoints) point = e.numPoints ? e.numPoints - 1 : 0;
    st.pos = (uint8_t)point;
}

// Lxx. The panning envelope only moves when the volume envelope has
// sustain on, an FT2 bug OpenMPT keeps.
void XmPlayer::setEnvelopePos(XmChannel& c, uint8_t param) {
    const XmInstrument* in = instrumentOf(c.envInst);
    if (!in) return;
    if (in->volEnv.enabled()) envSetPos(in->volEnv, c.volEnv, param);
    if (in->volEnv.sustainOn() && in->panEnv.numPoints) envSetPos(in->panEnv, c.panEnv, param);
}

void XmPlayer::tickZeroEffects(int ch) {
    XmChannel& c = _channels[ch];
    uint8_t v = c.volCol, vx = v & 0x0F;
    switch (v >> 4) {
        case 0x1: case 0x2: case 0x3: case 0x4: case 0x5:
            if (v <= 0x50) c.realVol = c.outVol = (uint8_t)(v - 0x10); // 0x51-0x5F do nothing, as FT2
            break;
        case 0x8: c.realVol = c.outVol = clampVol(c.realVol - vx); break;
        case 0x9: c.realVol = c.outVol = clampVol(c.realVol + vx); break;
        case 0xA: c.vibSpeed = (uint8_t)(vx << 2); break;
        case 0xC: c.outPan = (uint8_t)(vx << 4); break;
        default: break;
    }

    uint8_t p = c.eff, x = p & 0x0F;
    switch (c.effTyp) {
        case 0x08: c.outPan = p; break;
        case 0x0B:
            _positionJumpPending = true;
            _positionJumpTarget = p;
            break;
        case 0x0C: c.realVol = c.outVol = clampVol(p); break;
        case 0x0D: {
            int row = (p >> 4) * 10 + x;
            _patternBreakPending = true;
            _patternBreakRow = row <= 63 ? row : 0;
            break;
        }
        case 0x0E:
            switch (p >> 4) {
                case 0x1: { // E1x fine porta up
                    if (!x) x = c.fPortaUpSpeed;
                    c.fPortaUpSpeed = x;
                    int per = (int)c.realPeriod - x * 4;
                    c.outPeriod = c.realPeriod = (uint16_t)(per < 1 ? 1 : per);
                    break;
                }
                case 0x2: { // E2x fine porta down
                    if (!x) x = c.fPortaDownSpeed;
                    c.fPortaDownSpeed = x;
                    int per = (int)c.realPeriod + x * 4;
                    c.outPeriod = c.realPeriod = (uint16_t)(per > MAX_PERIOD ? MAX_PERIOD : per);
                    break;
                }
                case 0x3: c.glissando = x != 0; break;
                case 0x4: c.waveCtrl = (uint8_t)((c.waveCtrl & 0xF0) | x); break;
                case 0x6: patternLoop(c.patternLoopRow, c.patternLoopCount, x); break;
                case 0x7: c.waveCtrl = (uint8_t)((c.waveCtrl & 0x0F) | (x << 4)); break;
                case 0xA:
                    if (!x) x = c.fVolUpSpeed;
                    c.fVolUpSpeed = x;
                    c.realVol = c.outVol = clampVol(c.realVol + x);
                    break;
                case 0xB:
                    if (!x) x = c.fVolDownSpeed;
                    c.fVolDownSpeed = x;
                    c.realVol = c.outVol = clampVol(c.realVol - x);
                    break;
                case 0xC:
                    if (!x) c.realVol = c.outVol = 0; // EC0 cuts at once
                    break;
                case 0xE: _pendingRowDelay = x; break;
                default: break; // E0x filter, E5x (in triggerNote), E8x and EFx do nothing
            }
            break;
        case 0x0F:
            if (p >= 0x20) _tempo = p;
            else if (p) _speed = p;
            break;
        case 0x10: _globalVolume = p > 64 ? 64 : p; break;
        case 0x15: setEnvelopePos(c, p); break;
        case 0x1B:
            if (x) c.retrigSpeed = x;
            if (p >> 4) c.retrigVol = p >> 4;
            // Counts on tick 0 too, unless the volume column sets a volume.
            if (v < 0x10 || v > 0x50) multiRetrig(c, true);
            break;
        case 0x21:
            if ((p >> 4) == 1) { // X1x extra fine porta up
                if (!x) x = c.ePortaUpSpeed;
                c.ePortaUpSpeed = x;
                int per = (int)c.realPeriod - x;
                c.outPeriod = c.realPeriod = (uint16_t)(per < 1 ? 1 : per);
            } else if ((p >> 4) == 2) { // X2x extra fine porta down
                if (!x) x = c.ePortaDownSpeed;
                c.ePortaDownSpeed = x;
                int per = (int)c.realPeriod + x;
                c.outPeriod = c.realPeriod = (uint16_t)(per > MAX_PERIOD ? MAX_PERIOD : per);
            }
            break;
        default: break;
    }
}

// Rxy: every y ticks counted across rows, change the volume by x and play
// the note again from the start. Tick 0 of a row with a note only counts.
void __not_in_flash_func(XmPlayer::multiRetrig)(XmChannel& c, bool tickZero) {
    uint8_t cnt = (uint8_t)(c.retrigCnt + 1);
    if (cnt < c.retrigSpeed || (tickZero && c.rowNote >= 1 && c.rowNote <= 96)) {
        c.retrigCnt = cnt;
        return;
    }
    c.retrigCnt = 0;
    int vol = c.realVol;
    switch (c.retrigVol) {
        case 0x1: vol -= 1; break;
        case 0x2: vol -= 2; break;
        case 0x3: vol -= 4; break;
        case 0x4: vol -= 8; break;
        case 0x5: vol -= 16; break;
        case 0x6: vol = (vol >> 1) + (vol >> 3) + (vol >> 4); break;
        case 0x7: vol >>= 1; break;
        case 0x9: vol += 1; break;
        case 0xA: vol += 2; break;
        case 0xB: vol += 4; break;
        case 0xC: vol += 8; break;
        case 0xD: vol += 16; break;
        case 0xE: vol = (vol >> 1) + vol; break;
        case 0xF: vol += vol; break;
        default: break;
    }
    c.realVol = c.outVol = clampVol(vol);
    if (c.volCol >= 0x10 && c.volCol <= 0x50) c.realVol = c.outVol = clampVol(c.volCol - 0x10);
    else if (c.volCol >= 0xC0 && c.volCol <= 0xCF) c.outPan = (uint8_t)((c.volCol & 0x0F) << 4);
    triggerNote(c, 0, 0, 0);
}

void __not_in_flash_func(XmPlayer::portamento)(XmChannel& c) {
    if (c.portaDir == 0) return;
    int p = c.realPeriod;
    if (c.portaDir > 1) {
        p -= c.portaSpeed;
        if (p <= (int)c.wantPeriod) { c.portaDir = 1; p = c.wantPeriod; }
    } else {
        p += c.portaSpeed;
        if (p >= (int)c.wantPeriod) { c.portaDir = 1; p = c.wantPeriod; }
    }
    c.realPeriod = (uint16_t)p;
    c.outPeriod = c.glissando ? relocateTon(c.realPeriod, 0, c) : c.realPeriod;
}

void __not_in_flash_func(XmPlayer::vibrato)(XmChannel& c) {
    uint8_t t = (c.vibPos >> 2) & 0x1F;
    switch (c.waveCtrl & 3) {
        case 0: t = VIB_TAB[t]; break;
        case 1:
            t = (uint8_t)(t << 3);
            if ((int8_t)c.vibPos < 0) t = (uint8_t)~t;
            break;
        default: t = 255; break;
    }
    int d = (t * c.vibDepth) >> 5;
    int p = (int8_t)c.vibPos < 0 ? (int)c.realPeriod - d : (int)c.realPeriod + d;
    c.outPeriod = (uint16_t)(p < 1 ? 1 : p > MAX_PERIOD ? MAX_PERIOD : p);
    c.vibPos = (uint8_t)(c.vibPos + c.vibSpeed);
}

void __not_in_flash_func(XmPlayer::volSlide)(XmChannel& c, uint8_t p) {
    if (!p) p = c.volSlideSpeed;
    c.volSlideSpeed = p;
    if (!(p & 0xF0)) c.realVol = clampVol(c.realVol - (p & 0x0F));
    else c.realVol = clampVol(c.realVol + (p >> 4));
    c.outVol = c.realVol;
}

// EDx on tick x: the held cell plays, its note (or the last one again),
// instrument volumes and the volume column's volume or pan.
void __not_in_flash_func(XmPlayer::noteDelayTrigger)(XmChannel& c) {
    triggerNote(c, c.rowNote, 0, 0);
    if (c.rowInst) resetVolumes(c);
    triggerInstrument(c);
    if (c.volCol >= 0x10 && c.volCol <= 0x50) c.realVol = c.outVol = clampVol(c.volCol - 0x10);
    else if (c.volCol >= 0xC0 && c.volCol <= 0xCF) c.outPan = (uint8_t)((c.volCol & 0x0F) << 4);
}

// The ticks after the first.
void __not_in_flash_func(XmPlayer::tickEffects)(int ch) {
    XmChannel& c = _channels[ch];
    uint8_t v = c.volCol, vx = v & 0x0F;
    switch (v >> 4) {
        case 0x6: c.realVol = c.outVol = clampVol(c.realVol - vx); break;
        case 0x7: c.realVol = c.outVol = clampVol(c.realVol + vx); break;
        case 0xB:
            if (vx) c.vibDepth = vx;
            vibrato(c);
            break;
        case 0xD: // FT2 slides D0 all the way left
            c.outPan = (vx == 0 || c.outPan < vx) ? 0 : (uint8_t)(c.outPan - vx);
            break;
        case 0xE: c.outPan = (uint8_t)(c.outPan + vx > 255 ? 255 : c.outPan + vx); break;
        case 0xF: portamento(c); break;
        default: break;
    }

    uint8_t p = c.eff, x = p & 0x0F;
    if (c.effTyp == 0 && p == 0) return;
    switch (c.effTyp) {
        case 0x00: {
            // FT2 counts the tick down from the speed; past 15 its table
            // runs into the vibrato table, which is 0 then not 1.
            int timer = _speed - _tick;
            int arp = timer < 16 ? timer % 3 : timer == 16 ? 0 : 2;
            if (arp == 0) c.outPeriod = c.realPeriod;
            else c.outPeriod = relocateTon(c.realPeriod, arp == 1 ? p >> 4 : x, c);
            break;
        }
        case 0x01: {
            if (!p) p = c.portaUpSpeed;
            c.portaUpSpeed = p;
            int per = (int)c.realPeriod - p * 4;
            c.outPeriod = c.realPeriod = (uint16_t)(per < 1 ? 1 : per);
            break;
        }
        case 0x02: {
            if (!p) p = c.portaDownSpeed;
            c.portaDownSpeed = p;
            int per = (int)c.realPeriod + p * 4;
            c.outPeriod = c.realPeriod = (uint16_t)(per > MAX_PERIOD ? MAX_PERIOD : per);
            break;
        }
        case 0x03: portamento(c); break;
        case 0x04:
            if (x) c.vibDepth = x;
            if (p & 0xF0) c.vibSpeed = (uint8_t)((p >> 4) << 2);
            vibrato(c);
            break;
        case 0x05: portamento(c); volSlide(c, p); break;
        case 0x06: vibrato(c); volSlide(c, p); break;
        case 0x07: {
            if (p & 0xF0) c.tremSpeed = (uint8_t)((p >> 4) << 2);
            if (x) c.tremDepth = x;
            uint8_t t = (c.tremPos >> 2) & 0x1F;
            switch ((c.waveCtrl >> 4) & 3) {
                case 0: t = VIB_TAB[t]; break;
                case 1:
                    t = (uint8_t)(t << 3);
                    if ((int8_t)c.vibPos < 0) t = (uint8_t)~t; // FT2 looks at the vibrato position here
                    break;
                default: t = 255; break;
            }
            int d = (t * c.tremDepth) >> 6;
            c.outVol = clampVol((int8_t)c.tremPos < 0 ? c.realVol - d : c.realVol + d);
            c.tremPos = (uint8_t)(c.tremPos + c.tremSpeed);
            break;
        }
        case 0x0A: volSlide(c, p); break;
        case 0x0E:
            switch (p >> 4) {
                case 0x9:
                    if (x && _tick % x == 0) { triggerNote(c, 0, 0, 0); triggerInstrument(c); }
                    break;
                case 0xC:
                    if (_tick == x) c.realVol = c.outVol = 0;
                    break;
                case 0xD:
                    if (_tick == x) noteDelayTrigger(c);
                    break;
                default: break;
            }
            break;
        case 0x11: {
            if (!p) p = c.globVolSlideSpeed;
            c.globVolSlideSpeed = p;
            int g = _globalVolume;
            if (p >> 4) g += p >> 4;
            else g -= p & 0x0F;
            _globalVolume = (uint8_t)(g < 0 ? 0 : g > 64 ? 64 : g);
            break;
        }
        case 0x14:
            if (_tick == (p & 31)) keyOff(c);
            break;
        case 0x19: {
            if (!p) p = c.panSlideSpeed;
            c.panSlideSpeed = p;
            int pan = c.outPan;
            if (!(p & 0xF0)) pan -= p;
            else pan += p >> 4;
            c.outPan = (uint8_t)(pan < 0 ? 0 : pan > 255 ? 255 : pan);
            break;
        }
        case 0x1B: multiRetrig(c, false); break;
        case 0x1D: {
            if (!p) p = c.tremorParam;
            c.tremorParam = p;
            uint8_t sign = c.tremorPos & 0x80, data = (uint8_t)((c.tremorPos & 0x7F) - 1);
            if ((int8_t)data < 0) {
                if (sign == 0x80) { sign = 0; data = p & 0x0F; }
                else { sign = 0x80; data = p >> 4; }
            }
            c.tremorPos = (uint8_t)(sign | data);
            c.outVol = sign == 0x80 ? c.realVol : 0;
            break;
        }
        default: break;
    }
}

void XmPlayer::advanceRow() {
    // EEx repeats the row without its notes, running the effects of the
    // ticks after the first on its first tick too, as FT2.
    if (_rowDelay) {
        _rowDelay--;
        for (int ch = 0; ch < _numChannels; ch++) tickEffects(ch);
        return;
    }
    int openRows = (_openPatternNum >= 0 && _openPatternNum < MAX_PATTERNS) ? _patternRowCounts[_openPatternNum] : 64;
    stepPosition(openRows, MAX_ROWS - 1);
    // No restart: the song stops at the end of the order list, as MOD and S3M.
    if (_orderPos < 0 || _orderPos >= _songLength) {
        _state = STATE_DONE;
        return;
    }
    int patternNum = _orderTable[_orderPos];
    if (_row >= _patternRowCounts[patternNum]) _row = 0;
    if (patternNum != _openPatternNum) openPatternForScan(patternNum);
    advanceToRow(_row);
    _rowDelay = _pendingRowDelay;
    _pendingRowDelay = 0;
}

void __not_in_flash_func(XmPlayer::processTickEffects)() {
    for (int ch = 0; ch < _numChannels; ch++) tickEffects(ch);
}

// FT2's envelope step: count a tick, and at each point take its value and
// work out the step to the next, with the loop and sustain rules.
int __not_in_flash_func(XmPlayer::envTick)(const XmEnvelope& e, XmEnvState& st, bool sustainActive) {
    bool didIP = false;
    uint8_t pos = st.pos < e.numPoints ? st.pos : (uint8_t)(e.numPoints - 1);
    if (++st.cnt == e.pointX[pos]) {
        st.amp = (int16_t)(e.pointY[pos] << 8);
        pos++;
        if (e.loopOn()) {
            pos--;
            if (pos == e.loopEndPoint && (!e.sustainOn() || pos != e.sustainPoint || sustainActive)) {
                pos = e.loopStartPoint;
                st.cnt = e.pointX[pos];
                st.amp = (int16_t)(e.pointY[pos] << 8);
            }
            pos++;
        }
        if (pos < e.numPoints) {
            if (e.sustainOn() && sustainActive && pos - 1 == e.sustainPoint) {
                st.ip = 0;
            } else {
                st.pos = pos;
                st.ip = 0;
                if (e.pointX[pos] > e.pointX[pos - 1]) {
                    st.ip = (int16_t)(((e.pointY[pos] - e.pointY[pos - 1]) << 8) / (e.pointX[pos] - e.pointX[pos - 1]));
                    didIP = true;
                }
            }
        } else {
            st.ip = 0;
        }
    }
    if (!didIP) {
        int a = st.amp + st.ip;
        if (a > 64 * 256) { a = 64 * 256; st.ip = 0; }
        if (a < 0) { a = 0; st.ip = 0; }
        st.amp = (int16_t)a;
    }
    return st.amp >> 8;
}

// Every tick, after the row or effects: fadeout, envelopes and
// auto-vibrato give what the mixer plays.
void __not_in_flash_func(XmPlayer::tickEnd)() {
    for (int ch = 0; ch < _numChannels; ch++) {
        XmChannel& c = _channels[ch];
        const XmInstrument* in = instrumentOf(c.envInst);
        if (!c.sustainActive) {
            if (c.fadeOutAmp >= c.fadeOutSpeed) c.fadeOutAmp = (uint16_t)(c.fadeOutAmp - c.fadeOutSpeed);
            else c.fadeOutAmp = c.fadeOutSpeed = 0;
            if (c.fadeOutAmp == 0) c.havePair = false; // faded out, stop the voice
        }
        uint32_t env = 64;
        int pan = c.outPan;
        if (in) {
            if (in->volEnv.enabled()) env = (uint32_t)envTick(in->volEnv, c.volEnv, c.sustainActive);
            if (in->panEnv.enabled()) {
                int pe = envTick(in->panEnv, c.panEnv, c.sustainActive) - 32;
                int room = 128 - (pan >= 128 ? pan - 128 : 128 - pan);
                pan += pe * room / 32;
                pan = pan < 0 ? 0 : pan > 255 ? 255 : pan;
            }
        }
        c.finalVol = (uint16_t)(((uint32_t)c.outVol * env * c.fadeOutAmp) >> 15);
        c.finalPan = (uint8_t)pan;

        uint32_t period = c.outPeriod;
        if (in && in->vibDepth && period) {
            uint16_t amp;
            if (c.eVibSweep) {
                amp = c.eVibSweep;
                if (c.sustainActive) {
                    amp = (uint16_t)(amp + c.eVibAmp);
                    if ((amp >> 8) > in->vibDepth) { amp = (uint16_t)(in->vibDepth << 8); c.eVibSweep = 0; }
                    c.eVibAmp = amp;
                }
            } else {
                amp = c.eVibAmp;
            }
            c.eVibPos = (uint8_t)(c.eVibPos + in->vibRate);
            int val;
            switch (in->vibType) {
                case 1: val = c.eVibPos > 127 ? 64 : -64; break;
                case 2: val = (((c.eVibPos >> 1) + 64) & 127) - 64; break;
                case 3: val = (((0 - (c.eVibPos >> 1)) + 64) & 127) - 64; break;
                default: val = AUTOVIB_SINE[c.eVibPos]; break;
            }
            // Rounds toward zero as OpenMPT does; flooring sharpens long notes.
            int d = val * (int)amp;
            int p = (int)period + (d < 0 ? -((-d) >> 14) : d >> 14);
            period = (p < 0 || p > MAX_PERIOD) ? 0 : (uint32_t)p;
        }
        if (period != c.stepPeriod) {
            c.stepPeriod = (uint16_t)period;
            c.step23 = periodToStep23(period);
        }
        // The step's 7 bits below 16.16 are spread over ticks, so a long
        // note keeps in phase with OpenMPT's finer step.
        uint32_t acc = c.stepAcc + (c.step23 & 127);
        c.sampleStep16 = (c.step23 >> 7) + (acc >> 7);
        c.stepAcc = (uint8_t)(acc & 127);
    }
}

void __not_in_flash_func(XmPlayer::voiceGains)(const TrackerVoice& v, int32_t& l, int32_t& r) {
    const XmChannel& c = static_cast<const XmChannel&>(v);
    if (_linearPan) {
        // 32768 at full volume on one side; XM samples stay within int16.
        l = (int32_t)(((uint32_t)c.finalVol * (256u - c.finalPan)) >> 5);
        r = (int32_t)(((uint32_t)c.finalVol * c.finalPan) >> 5);
        return;
    }
    l = (int32_t)(((uint32_t)c.finalVol * PAN_SQRT[256 - c.finalPan]) >> 12);
    r = (int32_t)(((uint32_t)c.finalVol * PAN_SQRT[c.finalPan]) >> 12);
}
