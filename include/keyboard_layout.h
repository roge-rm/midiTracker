#pragma once

// Layout for the on-screen QWERTY name-entry keyboard, shared between
// ui.cpp (rendering) and main.cpp (input handling / editing) so both
// agree on what each grid cell means. Rows 0-3 are character keys
// (digits, then the three standard QWERTY letter rows, plus _ and -);
// row 4 holds SPACE (itself just a KEY_CHAR with ch==' '), DEL
// (backspace the last typed character), and OK (submit the name).
enum KeyKind { KEY_CHAR, KEY_DEL, KEY_OK };

struct KeyDef {
    KeyKind kind;
    char ch;           // meaningful for KEY_CHAR only
    const char* label; // what's drawn on the key
};

const KeyDef KEYBOARD_ROW0[] = {
    {KEY_CHAR, '1', "1"}, {KEY_CHAR, '2', "2"}, {KEY_CHAR, '3', "3"}, {KEY_CHAR, '4', "4"}, {KEY_CHAR, '5', "5"},
    {KEY_CHAR, '6', "6"}, {KEY_CHAR, '7', "7"}, {KEY_CHAR, '8', "8"}, {KEY_CHAR, '9', "9"}, {KEY_CHAR, '0', "0"},
};
const KeyDef KEYBOARD_ROW1[] = {
    {KEY_CHAR, 'Q', "Q"}, {KEY_CHAR, 'W', "W"}, {KEY_CHAR, 'E', "E"}, {KEY_CHAR, 'R', "R"}, {KEY_CHAR, 'T', "T"},
    {KEY_CHAR, 'Y', "Y"}, {KEY_CHAR, 'U', "U"}, {KEY_CHAR, 'I', "I"}, {KEY_CHAR, 'O', "O"}, {KEY_CHAR, 'P', "P"},
};
const KeyDef KEYBOARD_ROW2[] = {
    {KEY_CHAR, 'A', "A"}, {KEY_CHAR, 'S', "S"}, {KEY_CHAR, 'D', "D"}, {KEY_CHAR, 'F', "F"}, {KEY_CHAR, 'G', "G"},
    {KEY_CHAR, 'H', "H"}, {KEY_CHAR, 'J', "J"}, {KEY_CHAR, 'K', "K"}, {KEY_CHAR, 'L', "L"},
};
const KeyDef KEYBOARD_ROW3[] = {
    {KEY_CHAR, 'Z', "Z"}, {KEY_CHAR, 'X', "X"}, {KEY_CHAR, 'C', "C"}, {KEY_CHAR, 'V', "V"}, {KEY_CHAR, 'B', "B"},
    {KEY_CHAR, 'N', "N"}, {KEY_CHAR, 'M', "M"}, {KEY_CHAR, '_', "_"}, {KEY_CHAR, '-', "-"},
};
const KeyDef KEYBOARD_ROW4[] = {
    {KEY_CHAR, ' ', "SPACE"}, {KEY_DEL, 0, "DEL"}, {KEY_OK, 0, "OK"},
};

const KeyDef* const KEYBOARD_ROWS[] = {
    KEYBOARD_ROW0, KEYBOARD_ROW1, KEYBOARD_ROW2, KEYBOARD_ROW3, KEYBOARD_ROW4,
};
const int KEYBOARD_ROW_LENS[] = {10, 10, 9, 9, 3};
const int KEYBOARD_ROW_COUNT = 5;
