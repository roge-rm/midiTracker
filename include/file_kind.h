#pragma once

// Meaningless for directories. FILE_MID/FILE_SYX/FILE_WAV/FILE_MOD/
// FILE_S3M/FILE_XM/FILE_IT/FILE_MP3, see SdBrowser's isMidiFilename()/isSysExFilename()/isWavFilename()/
// isModFilename()/isS3mFilename()/isXmFilename()/isItFilename()/isMp3Filename().
enum FileKind { FILE_MID, FILE_SYX, FILE_WAV, FILE_MOD, FILE_S3M, FILE_XM, FILE_IT, FILE_MP3 };
