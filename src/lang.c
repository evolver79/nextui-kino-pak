// Language labels to one code. Subtitle and audio tracks name their language in many ways: ISO 639-1 ("en"), both
// ISO 639-2 forms ("ger"/"deu"), English names ("English" from Jellyfin), native names ("Norsk", "Deutsch"), and with
// extras ("en-US", "English (SDH)", "pt_BR"). lang_code() turns any of them into the one code in the table below, so a
// preference and a track compare code to code, whatever language.
#include "kino.h"
#include <string.h>
#include <ctype.h>

// code | everything else it can be called (lowercase) ; label: shown in Options, in the language itself where that's
// Latin script (Kino's font, Geist, has no Cyrillic, Greek or CJK: those get their English name)
static const struct { const char *code, *alts, *label; } L[] = {
  { "eng", "en|english|engelsk", "ENGLISH" },
  { "nor", "no|nb|nn|nob|nno|norwegian|norsk|bokmal|bokmål|nynorsk|norwegian bokmal|norwegian bokmål", "NORSK" },
  { "swe", "sv|swedish|svenska|svensk", "SVENSKA" },
  { "dan", "da|danish|dansk", "DANSK" },
  { "fin", "fi|finnish|suomi", "SUOMI" },
  { "isl", "is|ice|icelandic|islenska|íslenska", "ÍSLENSKA" },
  { "deu", "de|ger|german|deutsch|tysk", "DEUTSCH" },
  { "fra", "fr|fre|french|francais|français|fransk", "FRANÇAIS" },
  { "spa", "es|spanish|espanol|español|castellano|spansk", "ESPAÑOL" },
  { "por", "pt|portuguese|portugues|português", "PORTUGUÊS" },
  { "ita", "it|italian|italiano", "ITALIANO" },
  { "nld", "nl|dut|dutch|nederlands|flemish|vlaams", "NEDERLANDS" },
  { "pol", "pl|polish|polski", "POLSKI" },
  { "ces", "cs|cze|czech|cestina|čeština", "ČEŠTINA" },
  { "slk", "sk|slo|slovak|slovencina|slovenčina", "SLOVENČINA" },
  { "hun", "hu|hungarian|magyar", "MAGYAR" },
  { "ron", "ro|rum|romanian|romana|română", "ROMÂNĂ" },
  { "bul", "bg|bulgarian|български", "BULGARIAN" },
  { "hrv", "hr|croatian|hrvatski", "HRVATSKI" },
  { "srp", "sr|serbian|srpski|српски", "SRPSKI" },
  { "slv", "sl|slovenian|slovene|slovenscina|slovenščina", "SLOVENŠČINA" },
  { "ell", "el|gre|greek|ελληνικά", "GREEK" },
  { "tur", "tr|turkish|turkce|türkçe", "TÜRKÇE" },
  { "rus", "ru|russian|русский", "RUSSIAN" },
  { "ukr", "uk|ukrainian|українська", "UKRAINIAN" },
  { "est", "et|estonian|eesti", "EESTI" },
  { "lav", "lv|latvian|latviesu|latviešu", "LATVIEŠU" },
  { "lit", "lt|lithuanian|lietuviu|lietuvių", "LIETUVIŲ" },
  { "ara", "ar|arabic|العربية", "ARABIC" },
  { "heb", "he|iw|hebrew|עברית", "HEBREW" },
  { "fas", "fa|per|persian|farsi", "FARSI" },
  { "hin", "hi|hindi", "HINDI" },
  { "urd", "ur|urdu", "URDU" },
  { "ben", "bn|bengali|bangla", "BANGLA" },
  { "tam", "ta|tamil", "TAMIL" },
  { "tel", "te|telugu", "TELUGU" },
  { "tha", "th|thai", "THAI" },
  { "vie", "vi|vietnamese|tieng viet|tiếng việt", "TIẾNG VIỆT" },
  { "ind", "id|indonesian|bahasa indonesia", "INDONESIA" },
  { "msa", "ms|may|malay|bahasa melayu", "MELAYU" },
  { "fil", "tl|tgl|filipino|tagalog", "FILIPINO" },
  { "zho", "zh|chi|chinese|mandarin|cantonese|中文", "CHINESE" },
  { "jpn", "ja|japanese|日本語", "JAPANESE" },
  { "kor", "ko|korean|한국어", "KOREAN" },
  { "cat", "ca|catalan|catala|català", "CATALÀ" },
  { "eus", "eu|baq|basque|euskara", "EUSKARA" },
  { "glg", "gl|galician|galego", "GALEGO" },
};

// The first word of a label, lowercase: "English (SDH)" -> "english", "en-US" -> "en", "pt_BR" -> "pt".
// Non-ASCII bytes count as letters (native names); two words are kept for "norwegian bokmal" and the like.
static void first_words(const char *s, char *out, size_t n) {
  size_t o = 0; int words = 0;
  while (*s && !(isalpha((unsigned char)*s) || (unsigned char)*s >= 0x80)) s++;
  while (*s && o + 1 < n) {
    unsigned char c = (unsigned char)*s;
    if (isalpha(c) || c >= 0x80) out[o++] = (char)tolower(c);
    else if (c == ' ' && words == 0 && s[1] && (isalpha((unsigned char)s[1]) || (unsigned char)s[1] >= 0x80)) { out[o++] = ' '; words++; }
    else break;
    s++;
  }
  out[o] = 0;
}

static int in_list(const char *word, const char *alts) {
  size_t wl = strlen(word);
  for (const char *p = alts; *p; ) {
    const char *e = strchr(p, '|'); size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l == wl && !strncmp(p, word, l)) return 1;
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

static int find(const char *s) {
  if (!s || !*s) return -1;
  char w[48]; first_words(s, w, sizeof w);
  for (int pass = 0; pass < 2; pass++) {   // two words ("norwegian bokmal"), then the first word alone
    if (!*w) return -1;
    for (unsigned i = 0; i < sizeof L / sizeof *L; i++)
      if (!strcmp(w, L[i].code) || in_list(w, L[i].alts)) return (int)i;
    char *sp = strchr(w, ' '); if (!sp) break; *sp = 0;
  }
  return -1;
}

const char *lang_code(const char *s) { int i = find(s); return i < 0 ? NULL : L[i].code; }
int lang_count(void) { return (int)(sizeof L / sizeof *L); }
const char *lang_code_at(int i) { return i >= 0 && i < lang_count() ? L[i].code : ""; }
int lang_index(const char *code) { for (int i = 0; code && i < lang_count(); i++) if (!strcmp(code, L[i].code)) return i; return -1; }
const char *lang_label(const char *s) { int i = find(s); return i < 0 ? NULL : L[i].label; }
