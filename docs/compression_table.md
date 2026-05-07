# Compression Ratio

| Codec   | Index | Block info | Lexicon | Doc info | Posting store | vs Raw32 |
| ------- | ----- | ---------- | ------- | -------- | ------------- | -------- |
| Raw32 | 2.65 GB | 79.50 MB | 44.79 MB | 115.60 MB | 2.73 GB | 1.000x |
| VarByte | 788.12 MB | 79.50 MB | 43.50 MB | 115.60 MB | 867.62 MB | 0.310x |

VarByte saves 69.0% of posting-store bytes versus Raw32.
