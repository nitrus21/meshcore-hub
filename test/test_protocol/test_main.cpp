// Host unit tests: pio test -e native
// Frames are built byte by byte from the layouts in MeshCore's MyMesh.cpp.
#include "../../src/setup/WifiPick.h"
#include <unity.h>
#include <string.h>
#include "../../src/core/MeshClient.h"
#include "../../src/core/Route.h"
#include "../../src/protocol/Frames.h"
#include "../../src/transport/TcpFramer.h"

using namespace mc;

static void test_route_parse() {
  route::Parsed p = route::parse("@Marie on sort vers 18 h");
  TEST_ASSERT_TRUE(p.routed);
  TEST_ASSERT_FALSE(p.list);
  TEST_ASSERT_EQUAL_STRING("marie", p.name.c_str());
  TEST_ASSERT_EQUAL_STRING("on sort vers 18 h", p.body.c_str());

  p = route::parse("  @marie:  salut  \n");   // spaces around, colon after the name
  TEST_ASSERT_EQUAL_STRING("marie", p.name.c_str());
  TEST_ASSERT_EQUAL_STRING("salut", p.body.c_str());

  p = route::parse("@[Marie Dupont] salut");  // mention of the MeshCore apps: not a valid name
  TEST_ASSERT_TRUE(p.routed);
  TEST_ASSERT_EQUAL_STRING("marie dupont", p.name.c_str());
  TEST_ASSERT_FALSE(route::validName(p.name));
  TEST_ASSERT_EQUAL_STRING("salut", p.body.c_str());

  p = route::parse("@[Marie], salut");
  TEST_ASSERT_EQUAL_STRING("marie", p.name.c_str());
  TEST_ASSERT_EQUAL_STRING("salut", p.body.c_str());

  p = route::parse("@\xC3\x89lodie bonjour \xC3\xA9t\xC3\xA9");  // accents: folded in the name only
  TEST_ASSERT_EQUAL_STRING("elodie", p.name.c_str());
  TEST_ASSERT_EQUAL_STRING("bonjour \xC3\xA9t\xC3\xA9", p.body.c_str());

  p = route::parse("@marie");
  TEST_ASSERT_TRUE(p.routed);
  TEST_ASSERT_FALSE(p.list);
  TEST_ASSERT_TRUE(p.body.empty());

  TEST_ASSERT_TRUE(route::parse("@").list);
  TEST_ASSERT_TRUE(route::parse("@?").list);
  TEST_ASSERT_TRUE(route::parse("@ salut").list);
  TEST_ASSERT_TRUE(route::parse("@[").list);

  TEST_ASSERT_FALSE(route::parse("salut @marie").routed);  // only at the start
  TEST_ASSERT_FALSE(route::parse("").routed);
  TEST_ASSERT_FALSE(route::parse("   ").routed);
}

static void test_route_command() {
  std::string c, a;
  TEST_ASSERT_TRUE(route::command("/msg Paul salut toi", "MonBot", c, a));
  TEST_ASSERT_EQUAL_STRING("msg", c.c_str());
  TEST_ASSERT_EQUAL_STRING("Paul salut toi", a.c_str());
  TEST_ASSERT_TRUE(route::command("/MSG@monbot   Paul salut", "MonBot", c, a));
  TEST_ASSERT_EQUAL_STRING("msg", c.c_str());
  TEST_ASSERT_EQUAL_STRING("Paul salut", a.c_str());
  TEST_ASSERT_TRUE(route::command("/msg@MonBot", "MonBot", c, a));
  TEST_ASSERT_EQUAL_STRING("", a.c_str());
  TEST_ASSERT_TRUE(route::command("/msg\nPaul salut", "MonBot", c, a));
  TEST_ASSERT_EQUAL_STRING("Paul salut", a.c_str());
  TEST_ASSERT_TRUE(route::command("/msg ", "MonBot", c, a));
  TEST_ASSERT_EQUAL_STRING("", a.c_str());
  TEST_ASSERT_TRUE(route::command("/msg@bot x", "", c, a));  // name of the bot not known yet
  TEST_ASSERT_FALSE(route::command("/msg@AutreBot Paul salut", "MonBot", c, a));
  TEST_ASSERT_FALSE(route::command("salut /msg", "MonBot", c, a));
  TEST_ASSERT_FALSE(route::command("/ salut", "MonBot", c, a));
  TEST_ASSERT_FALSE(route::command("", "MonBot", c, a));
}

static void test_route_several() {
  route::Parsed p = route::parse("@Marie @paul on sort");
  TEST_ASSERT_EQUAL_UINT(2, p.names.size());
  TEST_ASSERT_EQUAL_STRING("marie", p.name.c_str());
  TEST_ASSERT_EQUAL_STRING("paul", p.names[1].c_str());
  TEST_ASSERT_EQUAL_STRING("on sort", p.body.c_str());

  p = route::parse("@marie, @paul: salut");
  TEST_ASSERT_EQUAL_UINT(2, p.names.size());
  TEST_ASSERT_EQUAL_STRING("salut", p.body.c_str());

  p = route::parse("@[Marie] @[Paul] salut");
  TEST_ASSERT_EQUAL_UINT(2, p.names.size());
  TEST_ASSERT_EQUAL_STRING("paul", p.names[1].c_str());

  p = route::parse("@marie @MARIE salut");  // the same person once
  TEST_ASSERT_EQUAL_UINT(1, p.names.size());

  p = route::parse("@marie: @paul viendra");  // a colon ends the recipients
  TEST_ASSERT_EQUAL_UINT(1, p.names.size());
  TEST_ASSERT_EQUAL_STRING("@paul viendra", p.body.c_str());

  p = route::parse("@[Marie] : @paul viendra");
  TEST_ASSERT_EQUAL_UINT(1, p.names.size());
  TEST_ASSERT_EQUAL_STRING("@paul viendra", p.body.c_str());

  p = route::parse("@marie @paul");  // no text
  TEST_ASSERT_EQUAL_UINT(2, p.names.size());
  TEST_ASSERT_TRUE(p.body.empty());

  p = route::parse("@marie @ 18 h");  // a lone @ is text
  TEST_ASSERT_EQUAL_UINT(1, p.names.size());
  TEST_ASSERT_EQUAL_STRING("@ 18 h", p.body.c_str());

  p = route::parse("@marie rdv avec @paul");  // only in front of the text
  TEST_ASSERT_EQUAL_UINT(1, p.names.size());
  TEST_ASSERT_EQUAL_STRING("rdv avec @paul", p.body.c_str());

  p = route::parse("@a @b @c @d @e @f texte");
  TEST_ASSERT_TRUE(p.names.size() > route::MAX_TARGETS);

  TEST_ASSERT_TRUE(route::parse("@ @marie").list);  // lone @ first: still the list
  TEST_ASSERT_EQUAL_UINT(1, route::parse("@").names.size());
}

static void test_route_fit() {
  std::vector<std::string> n{"marie", "paul", "equipe"};
  TEST_ASSERT_EQUAL_STRING("@marie @paul @equipe", route::fit(n, 160).c_str());
  TEST_ASSERT_EQUAL_STRING("@marie @paul @equipe", route::fit(n, 20).c_str());  // exactly
  TEST_ASSERT_EQUAL_STRING("@marie @paul +1", route::fit(n, 19).c_str());
  TEST_ASSERT_EQUAL_STRING("@marie +2", route::fit(n, 10).c_str());
  TEST_ASSERT_EQUAL_STRING(" +3", route::fit(n, 3).c_str());
  TEST_ASSERT_TRUE(route::fit({}, 160).empty());
  std::vector<std::string> many(16, std::string(20, 'a'));
  TEST_ASSERT_TRUE(route::fit(many, 130).size() <= 130);
  TEST_ASSERT_TRUE(route::looksLikeInvite("0123456789abcdef"));
  TEST_ASSERT_FALSE(route::looksLikeInvite("hub"));
  TEST_ASSERT_FALSE(route::looksLikeInvite("0123456789ABCDEF"));
  TEST_ASSERT_FALSE(route::looksLikeInvite("0123456789abcde"));
}

static void test_route_names() {
  TEST_ASSERT_TRUE(route::validName("marie"));
  TEST_ASSERT_TRUE(route::validName("equipe-1_b"));
  TEST_ASSERT_FALSE(route::validName(""));
  TEST_ASSERT_FALSE(route::validName("Marie"));     // not folded
  TEST_ASSERT_FALSE(route::validName("marie!"));
  TEST_ASSERT_FALSE(route::validName("?"));
  TEST_ASSERT_TRUE(route::validName(std::string(route::MAX_NAME, 'a')));
  TEST_ASSERT_FALSE(route::validName(std::string(route::MAX_NAME + 1, 'a')));
  TEST_ASSERT_EQUAL_STRING("elodie", route::fold("\xC3\x89LODIE").c_str());
  TEST_ASSERT_EQUAL_STRING("francois", route::fold("Fran\xC3\xA7ois").c_str());
  TEST_ASSERT_EQUAL_STRING("noel", route::fold("No\xC3\xABl").c_str());
  TEST_ASSERT_EQUAL_STRING("\xE2\x82\xAC", route::fold("\xE2\x82\xAC").c_str());  // other UTF-8 untouched
}

static void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
static void put16(Bytes& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
static void putStr(Bytes& b, const char* s, size_t fixed) {
  size_t l = strlen(s);
  for (size_t i = 0; i < fixed; i++) b.push_back(i < l ? (uint8_t)s[i] : 0);
}

static Bytes contactFrame(uint8_t code, uint8_t keyByte, uint8_t type, const char* name, uint8_t pathLen = 0xFF) {
  Bytes b{code};
  for (int i = 0; i < 32; i++) b.push_back((uint8_t)(keyByte + i));
  b.push_back(type);
  b.push_back(0);        // flags
  b.push_back(pathLen);  // out_path_len
  for (int i = 0; i < 64; i++) b.push_back((uint8_t)(i < 2 ? 0xA0 + i : 0));
  putStr(b, name, 32);
  put32(b, 1700000000);
  put32(b, (uint32_t)48856614);   // lat 48.856614
  put32(b, (uint32_t)2352222);    // lon 2.352222
  put32(b, 1700000100);
  return b;
}

// ---------------------------------------------------------------- encoders

void test_encode_device_query() {
  Bytes f = enc::deviceQuery();
  TEST_ASSERT_EQUAL(2, f.size());
  TEST_ASSERT_EQUAL_HEX8(0x16, f[0]);
  TEST_ASSERT_EQUAL_HEX8(0x03, f[1]);
}

void test_encode_app_start() {
  Bytes f = enc::appStart("MeshCoreHub");
  TEST_ASSERT_EQUAL(8 + 11, f.size());
  TEST_ASSERT_EQUAL_HEX8(0x01, f[0]);
  TEST_ASSERT_EQUAL_HEX8(0x03, f[1]);
  for (int i = 2; i < 8; i++) TEST_ASSERT_EQUAL_HEX8(' ', f[i]);
  TEST_ASSERT_EQUAL_MEMORY("MeshCoreHub", &f[8], 11);
}

void test_encode_send_txt() {
  uint8_t prefix[6] = {1, 2, 3, 4, 5, 6};
  Bytes f = enc::sendTxtMsg(TXT_PLAIN, 1, 0x11223344, prefix, "Hi");
  TEST_ASSERT_EQUAL(14 + 2 - 1, f.size());  // 1+1+1+4+6 + text
  TEST_ASSERT_EQUAL_HEX8(CMD_SEND_TXT_MSG, f[0]);
  TEST_ASSERT_EQUAL_HEX8(0, f[1]);
  TEST_ASSERT_EQUAL_HEX8(1, f[2]);
  TEST_ASSERT_EQUAL_HEX8(0x44, f[3]);
  TEST_ASSERT_EQUAL_HEX8(0x11, f[6]);
  TEST_ASSERT_EQUAL_MEMORY(prefix, &f[7], 6);
  TEST_ASSERT_EQUAL_MEMORY("Hi", &f[13], 2);
}

void test_encode_channel_and_set_channel() {
  Bytes f = enc::sendChannelTxtMsg(2, 5, "yo");
  uint8_t exp[] = {3, 0, 2, 5, 0, 0, 0, 'y', 'o'};
  TEST_ASSERT_EQUAL(sizeof(exp), f.size());
  TEST_ASSERT_EQUAL_MEMORY(exp, f.data(), sizeof(exp));

  uint8_t secret[16];
  for (int i = 0; i < 16; i++) secret[i] = (uint8_t)i;
  Bytes s = enc::setChannel(1, "Friends", secret);
  TEST_ASSERT_EQUAL(2 + 32 + 16, s.size());
  TEST_ASSERT_EQUAL_HEX8(32, s[0]);
  TEST_ASSERT_EQUAL_HEX8(1, s[1]);
  TEST_ASSERT_EQUAL_MEMORY("Friends", &s[2], 7);
  TEST_ASSERT_EQUAL_HEX8(0, s[2 + 7]);
  TEST_ASSERT_EQUAL_MEMORY(secret, &s[34], 16);
}

void test_encode_misc() {
  uint8_t pub[32];
  for (int i = 0; i < 32; i++) pub[i] = (uint8_t)i;
  Bytes t = enc::telemetryReq(pub);
  TEST_ASSERT_EQUAL(4 + 32, t.size());
  TEST_ASSERT_EQUAL_HEX8(39, t[0]);
  TEST_ASSERT_EQUAL_MEMORY(pub, &t[4], 32);
  Bytes p = enc::pathDiscoveryReq(pub);
  TEST_ASSERT_EQUAL(2 + 32, p.size());
  TEST_ASSERT_EQUAL_HEX8(52, p[0]);
  TEST_ASSERT_EQUAL_HEX8(0, p[1]);
  Bytes r = enc::reboot();
  TEST_ASSERT_EQUAL(7, r.size());
  TEST_ASSERT_EQUAL_MEMORY("reboot", &r[1], 6);
  Bytes rp = enc::setRadioParams(869618, 62500, 8, 8, false);
  TEST_ASSERT_EQUAL(1 + 4 + 4 + 3, rp.size());
  TEST_ASSERT_EQUAL_HEX8(0xF2, rp[1]);  // 869618 = 0x000D44F2
  Bytes l = enc::sendLogin(pub, "a-very-long-password-xx");
  TEST_ASSERT_EQUAL(1 + 32 + 15, l.size());   // firmware keeps 15 chars max
}

// ---------------------------------------------------------------- decoders

void test_decode_device_info() {
  Bytes b{13, 14, 50, 8};
  put32(b, 123456);
  putStr(b, "15 Oct 2026", 12);
  putStr(b, "Heltec V3", 40);
  putStr(b, "v1.12.0", 20);
  b.push_back(0);
  b.push_back(1);
  DeviceInfo d;
  TEST_ASSERT_TRUE(dec::deviceInfo(b.data(), b.size(), d));
  TEST_ASSERT_EQUAL(14, d.fwVer);
  TEST_ASSERT_EQUAL(100, d.maxContacts);
  TEST_ASSERT_EQUAL(8, d.maxChannels);
  TEST_ASSERT_EQUAL(123456, d.blePin);
  TEST_ASSERT_EQUAL_STRING("Heltec V3", d.manufacturer.c_str());
  TEST_ASSERT_EQUAL_STRING("v1.12.0", d.fwVersion.c_str());
  TEST_ASSERT_EQUAL(1, d.pathHashMode);
}

void test_decode_self_info() {
  Bytes b{5, 1, 22, 30};
  for (int i = 0; i < 32; i++) b.push_back((uint8_t)(0xC0 + i));
  put32(b, (uint32_t)-33868820);  // lat
  put32(b, 151209290);
  b.push_back(1); b.push_back(2); b.push_back(0); b.push_back(1);
  put32(b, 869618);
  put32(b, 62500);
  b.push_back(8); b.push_back(8);
  const char* nm = "Hub Paris";
  b.insert(b.end(), nm, nm + strlen(nm));
  SelfInfo s;
  TEST_ASSERT_TRUE(dec::selfInfo(b.data(), b.size(), s));
  TEST_ASSERT_EQUAL(22, s.txPower);
  TEST_ASSERT_EQUAL_HEX8(0xC0, s.pubKey[0]);
  TEST_ASSERT_EQUAL(-33868820, s.lat);
  TEST_ASSERT_EQUAL(869618, s.freq);
  TEST_ASSERT_EQUAL(62500, s.bw);
  TEST_ASSERT_EQUAL(8, s.sf);
  TEST_ASSERT_EQUAL_STRING("Hub Paris", s.name.c_str());
}

void test_decode_contact() {
  Bytes b = contactFrame(RESP_CONTACT, 0x10, ADV_ROOM, "Salon", 0x42);
  TEST_ASSERT_EQUAL(148, b.size());
  Contact c;
  TEST_ASSERT_TRUE(dec::contact(b.data(), b.size(), c));
  TEST_ASSERT_EQUAL_HEX8(0x10, c.pubKey[0]);
  TEST_ASSERT_EQUAL(ADV_ROOM, c.type);
  TEST_ASSERT_EQUAL_STRING("Salon", c.name);
  TEST_ASSERT_EQUAL(2, pathHops(c.outPathLen));
  TEST_ASSERT_EQUAL(2, pathHashSize(c.outPathLen));
  TEST_ASSERT_EQUAL(4, pathByteLen(c.outPathLen));
  TEST_ASSERT_EQUAL(48856614, c.lat);
  TEST_ASSERT_EQUAL(1700000100u, c.lastmod);
  // a short frame must be rejected
  TEST_ASSERT_FALSE(dec::contact(b.data(), 100, c));
}

void test_decode_dm_v3() {
  Bytes b{16, (uint8_t)(int8_t)(30), 0, 0, 1, 2, 3, 4, 5, 6, 0xFF, 0};
  put32(b, 1700000500);
  const char* t = "Salut 👋";
  b.insert(b.end(), t, t + strlen(t));
  RecvMsg m;
  TEST_ASSERT_TRUE(dec::message(b.data(), b.size(), m));
  TEST_ASSERT_FALSE(m.isChannel);
  TEST_ASSERT_TRUE(m.hasSnr);
  TEST_ASSERT_EQUAL_FLOAT(7.5f, m.snr);
  TEST_ASSERT_EQUAL_HEX8(6, m.prefix[5]);
  TEST_ASSERT_EQUAL(-1, pathHops(m.pathLen));
  TEST_ASSERT_EQUAL(1700000500u, m.senderTs);
  TEST_ASSERT_EQUAL_STRING(t, m.text.c_str());
}

void test_decode_room_signed() {
  Bytes b{16, (uint8_t)(int8_t)(-12), 0, 0, 9, 9, 9, 9, 9, 9, 2, TXT_SIGNED_PLAIN};
  put32(b, 1700000600);
  b.push_back(0xAA); b.push_back(0xBB); b.push_back(0xCC); b.push_back(0xDD);
  const char* t = "hello room";
  b.insert(b.end(), t, t + strlen(t));
  RecvMsg m;
  TEST_ASSERT_TRUE(dec::message(b.data(), b.size(), m));
  TEST_ASSERT_TRUE(m.hasAuthor);
  TEST_ASSERT_EQUAL_HEX8(0xAA, m.author[0]);
  TEST_ASSERT_EQUAL_FLOAT(-3.0f, m.snr);
  TEST_ASSERT_EQUAL(2, pathHops(m.pathLen));
  TEST_ASSERT_EQUAL_STRING("hello room", m.text.c_str());
}

void test_decode_channel_v3_and_legacy() {
  Bytes b{17, 20, 0, 0, 0, 3, 0};
  put32(b, 1700000700);
  const char* t = "Bob: on se voit à 18h ?";
  b.insert(b.end(), t, t + strlen(t));
  RecvMsg m;
  TEST_ASSERT_TRUE(dec::message(b.data(), b.size(), m));
  TEST_ASSERT_TRUE(m.isChannel);
  TEST_ASSERT_EQUAL(0, m.channelIdx);
  TEST_ASSERT_EQUAL(3, pathHops(m.pathLen));
  TEST_ASSERT_EQUAL_STRING("Bob", m.channelSender().c_str());
  TEST_ASSERT_EQUAL_STRING("on se voit à 18h ?", m.channelBody().c_str());

  Bytes l{8, 1, 0xFF, 0};  // legacy code 8: no snr / reserved bytes
  put32(l, 42);
  l.push_back('x');
  TEST_ASSERT_TRUE(dec::message(l.data(), l.size(), m));
  TEST_ASSERT_FALSE(m.hasSnr);
  TEST_ASSERT_EQUAL(1, m.channelIdx);
  TEST_ASSERT_EQUAL_STRING("x", m.text.c_str());

  Bytes l7{7, 1, 2, 3, 4, 5, 6, 0xFF, 0};
  put32(l7, 43);
  l7.push_back('y');
  TEST_ASSERT_TRUE(dec::message(l7.data(), l7.size(), m));
  TEST_ASSERT_FALSE(m.isChannel);
  TEST_ASSERT_EQUAL_STRING("y", m.text.c_str());
}

void test_decode_sent_confirmed_login() {
  Bytes s{6, 1};
  put32(s, 0xDEADBEEF);
  put32(s, 4500);
  SentInfo si;
  TEST_ASSERT_TRUE(dec::sent(s.data(), s.size(), si));
  TEST_ASSERT_TRUE(si.flood);
  TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, si.tag);
  TEST_ASSERT_EQUAL(4500, si.timeoutMs);

  Bytes c{0x82};
  put32(c, 0xDEADBEEF);
  put32(c, 1234);
  SendConfirmed sc;
  TEST_ASSERT_TRUE(dec::sendConfirmed(c.data(), c.size(), sc));
  TEST_ASSERT_EQUAL(1234, sc.tripMs);

  Bytes lg{0x85, 1, 1, 2, 3, 4, 5, 6};
  put32(lg, 99);
  lg.push_back(3);
  lg.push_back(7);
  LoginResult lr;
  TEST_ASSERT_TRUE(dec::login(lg.data(), lg.size(), lr));
  TEST_ASSERT_TRUE(lr.ok);
  TEST_ASSERT_EQUAL(1, lr.permissions);
  TEST_ASSERT_EQUAL(99, lr.serverTs);
  TEST_ASSERT_EQUAL(3, lr.acl);
  Bytes lf{0x86, 0, 1, 2, 3, 4, 5, 6};
  TEST_ASSERT_TRUE(dec::login(lf.data(), lf.size(), lr));
  TEST_ASSERT_FALSE(lr.ok);
}

void test_decode_status_trace_batt_stats() {
  Bytes s{0x87, 0, 1, 2, 3, 4, 5, 6};
  put16(s, 4100); put16(s, 2); put16(s, (uint16_t)-110); put16(s, (uint16_t)-80);
  put32(s, 1000); put32(s, 900); put32(s, 55); put32(s, 86400);
  put32(s, 1); put32(s, 2); put32(s, 3); put32(s, 4);
  put16(s, 0); put16(s, (uint16_t)(int16_t)26); put16(s, 5); put16(s, 6); put32(s, 77);
  RepeaterStatus st;
  TEST_ASSERT_TRUE(dec::status(s.data(), s.size(), st));
  TEST_ASSERT_EQUAL(4100, st.battMv);
  TEST_ASSERT_EQUAL(-110, st.noiseFloor);
  TEST_ASSERT_EQUAL(86400, st.uptime);
  TEST_ASSERT_EQUAL_FLOAT(6.5f, st.lastSnr);
  TEST_ASSERT_FALSE(st.hasRecvErrors);

  // trace: path_len=2 hashes, flags=0 (1-byte hashes) -> 2 snrs + final
  Bytes t{0x89, 0, 2, 0};
  put32(t, 7); put32(t, 0);
  t.push_back(0x11); t.push_back(0x22);
  t.push_back(40); t.push_back((uint8_t)(int8_t)-8);
  t.push_back(12);
  TraceData td;
  TEST_ASSERT_TRUE(dec::trace(t.data(), t.size(), td));
  TEST_ASSERT_EQUAL(2, td.hashes.size());
  TEST_ASSERT_EQUAL(3, td.snrs.size());
  TEST_ASSERT_EQUAL_FLOAT(10.0f, td.snrs[0]);
  TEST_ASSERT_EQUAL_FLOAT(-2.0f, td.snrs[1]);
  TEST_ASSERT_EQUAL_FLOAT(3.0f, td.snrs[2]);

  Bytes b{12};
  put16(b, 3987); put32(b, 12); put32(b, 1024);
  BattStorage bs;
  TEST_ASSERT_TRUE(dec::battStorage(b.data(), b.size(), bs));
  TEST_ASSERT_EQUAL(3987, bs.mv);
  TEST_ASSERT_EQUAL(1024, bs.totalKb);

  Bytes r{24, 1};
  put16(r, (uint16_t)-115); r.push_back((uint8_t)-90); r.push_back(20); put32(r, 10); put32(r, 20);
  Stats sr;
  TEST_ASSERT_TRUE(dec::stats(r.data(), r.size(), sr));
  TEST_ASSERT_EQUAL(-115, sr.noiseFloor);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, sr.lastSnr);
}

void test_decode_path_discovery() {
  Bytes p{0x8D, 0, 1, 2, 3, 4, 5, 6, 2, 0xAA, 0xBB, 0x41, 0x01, 0x02};  // in path: 1 hop of 2 bytes
  PathDiscovery pd;
  TEST_ASSERT_TRUE(dec::pathDiscovery(p.data(), p.size(), pd));
  TEST_ASSERT_EQUAL(2, pd.outPath.size());
  TEST_ASSERT_EQUAL(2, pd.inPath.size());
  TEST_ASSERT_EQUAL(1, pathHops(pd.inPathLen));
}

// ---------------------------------------------------------------- helpers

void test_hashtag_secret() {
  // value from MeshCore docs/companion_protocol.md
  uint8_t s[16];
  hashtagSecret("#test", s);
  TEST_ASSERT_EQUAL_STRING("9cd8fcf22a47333b591d96a2b848b73f", toHex(s, 16).c_str());
  hashtagSecret("test", s);  // '#' is added when missing
  TEST_ASSERT_EQUAL_STRING("9cd8fcf22a47333b591d96a2b848b73f", toHex(s, 16).c_str());
  uint8_t h[32];
  sha256((const uint8_t*)"abc", 3, h);
  TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", toHex(h, 32).c_str());
  std::string longStr(200, 'a');
  sha256((const uint8_t*)longStr.data(), longStr.size(), h);  // multi-block path
  TEST_ASSERT_EQUAL_STRING("c2a908d98f5df987ade41b5fce213067efbcc21ef2240212a41e54b5e7c28ae5", toHex(h, 32).c_str());
}

void test_utf8_truncate_and_hex() {
  std::string s = "ééé";  // 6 bytes
  TEST_ASSERT_EQUAL_STRING("éé", utf8Truncate(s, 5).c_str());
  TEST_ASSERT_EQUAL_STRING("ééé", utf8Truncate(s, 6).c_str());
  uint8_t out[3];
  TEST_ASSERT_TRUE(fromHex("0aFf10", out, 3));
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[1]);
  TEST_ASSERT_FALSE(fromHex("0g", out, 1));
  TEST_ASSERT_EQUAL(0, fromHexVar("abc").size());
}

void test_tcp_framer() {
  TcpFramer f;
  uint8_t stream[] = {'x', 'y', '>', 3, 0, 0xA, 0xB, 0xC, '>', 1, 0, 0x83};
  int frames = 0;
  size_t lastLen = 0;
  for (uint8_t c : stream) {
    size_t n = f.push(c);
    if (n) { frames++; lastLen = n; }
    if (frames == 1 && n) TEST_ASSERT_EQUAL_HEX8(0xB, f.buf()[1]);
  }
  TEST_ASSERT_EQUAL(2, frames);
  TEST_ASSERT_EQUAL(1, lastLen);
  TEST_ASSERT_EQUAL_HEX8(0x83, f.buf()[0]);
  uint8_t hdr[3];
  TcpFramer::header(hdr, 300);
  TEST_ASSERT_EQUAL_HEX8('<', hdr[0]);
  TEST_ASSERT_EQUAL_HEX8(44, hdr[1]);
  TEST_ASSERT_EQUAL_HEX8(1, hdr[2]);
}

// ---------------------------------------------------------------- MeshClient with a fake radio

static uint32_t gNow = 1000;

// Minimal in-memory Companion radio: answers like MyMesh::handleCmdFrame.
class FakeRadio : public Transport {
 public:
  bool up = false;
  bool ackMessages = true;
  int muteChannel = -1;             // GET_CHANNEL for this slot gets no answer (lost frame)
  std::vector<Bytes> sent;          // every frame received from the client
  std::deque<Bytes> queued;         // offline message queue
  std::vector<Bytes> pendingPush;   // pushes to deliver on next poll
  FrameCb cb;
  uint32_t ackSeq = 100;

  bool connect() override { up = true; return true; }
  void disconnect() override { up = false; }
  bool isConnected() override { return up; }
  void onFrame(FrameCb c) override { cb = c; }
  const char* name() const override { return "fake"; }
  void reply(const Bytes& b) { cb(b.data(), b.size()); }

  void poll() override {
    auto p = pendingPush;
    pendingPush.clear();
    for (auto& b : p) reply(b);
  }

  bool sendFrame(const uint8_t* d, size_t n) override {
    Bytes f(d, d + n);
    sent.push_back(f);
    switch (d[0]) {
      case CMD_DEVICE_QUERY: {
        Bytes b{13, 14, 50, 2};
        put32(b, 123456);
        putStr(b, "today", 12); putStr(b, "Fake", 40); putStr(b, "v1.12.0", 20);
        b.push_back(0); b.push_back(0);
        reply(b);
        break;
      }
      case CMD_APP_START: {
        Bytes b{5, 1, 20, 22};
        for (int i = 0; i < 32; i++) b.push_back(0xEE);
        put32(b, 0); put32(b, 0);
        b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0);
        put32(b, 869618); put32(b, 62500); b.push_back(8); b.push_back(8);
        b.push_back('H'); b.push_back('u'); b.push_back('b');
        reply(b);
        break;
      }
      case CMD_SET_DEVICE_TIME: reply({0}); break;
      case CMD_GET_DEVICE_TIME: { Bytes b{9}; put32(b, 1750000000); reply(b); break; }
      case CMD_GET_CONTACTS: {
        Bytes s{2}; put32(s, 2); reply(s);
        reply(contactFrame(3, 0x10, ADV_CHAT, "Alice", 0x01));
        reply(contactFrame(3, 0x50, ADV_ROOM, "Room"));
        Bytes e{4}; put32(e, 1700000100); reply(e);
        break;
      }
      case CMD_GET_CHANNEL: {
        if (d[1] == muteChannel) break;
        if (d[1] == 0) {
          Bytes b{18, 0}; putStr(b, "Public", 32);
          for (int i = 0; i < 16; i++) b.push_back((uint8_t)i);
          reply(b);
        } else {
          reply({1, ERR_NOT_FOUND});
        }
        break;
      }
      case CMD_GET_BATT_AND_STORAGE: { Bytes b{12}; put16(b, 4000); put32(b, 1); put32(b, 2); reply(b); break; }
      case CMD_SYNC_NEXT_MESSAGE:
        if (queued.empty()) reply({10});
        else { reply(queued.front()); queued.pop_front(); }
        break;
      case CMD_SEND_TXT_MSG: {
        uint32_t ack = ++ackSeq;
        Bytes b{6, 0}; put32(b, ack); put32(b, 2000);
        reply(b);
        if (ackMessages) {
          Bytes c{0x82}; put32(c, ack); put32(c, 850);
          pendingPush.push_back(c);
        }
        break;
      }
      case CMD_SEND_CHANNEL_TXT_MSG: reply({0}); break;
      case CMD_RESET_PATH: reply({0}); break;
      case CMD_SEND_LOGIN: {
        Bytes b{6, 1}; put32(b, 0x50515253); put32(b, 3000);
        reply(b);
        Bytes ok{0x85, 0, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55};
        pendingPush.push_back(ok);
        break;
      }
      default: reply({1, ERR_UNSUPPORTED_CMD});
    }
    return true;
  }
};

struct Recorder : MeshListener {
  std::vector<RecvMsg> msgs;
  std::vector<std::pair<uint32_t, MsgStatus>> statuses;
  uint32_t lastRtt = 0;
  int logins = 0;
  bool contactsLoaded = false, channelsLoaded = false;
  void onMessage(const RecvMsg& m) override { msgs.push_back(m); }
  void onMsgStatus(uint32_t id, MsgStatus s, uint32_t rtt, uint8_t) override {
    statuses.push_back({id, s});
    if (s == MsgStatus::Delivered) lastRtt = rtt;
  }
  void onLogin(const LoginResult& r) override { if (r.ok) logins++; }
  void onContactsLoaded() override { contactsLoaded = true; }
  void onChannelsLoaded() override { channelsLoaded = true; }
};

static void run(MeshClient& c, int iterations = 50, uint32_t stepMs = 10) {
  for (int i = 0; i < iterations; i++) {
    c.loop();
    gNow += stepMs;
  }
}

void test_client_handshake_and_messages() {
  gNow = 1000;
  FakeRadio radio;
  Recorder rec;
  MeshClient client([] { return gNow; }, [] { return (uint32_t)1750000000; });
  client.setListener(&rec);
  client.setTransport(&radio);
  run(client);
  TEST_ASSERT_TRUE(client.ready());
  TEST_ASSERT_TRUE(rec.contactsLoaded);
  TEST_ASSERT_TRUE(rec.channelsLoaded);
  TEST_ASSERT_EQUAL(2, client.contactCount());
  TEST_ASSERT_EQUAL(1, client.channels().size());
  TEST_ASSERT_EQUAL_STRING("Hub", client.selfInfo().name.c_str());
  TEST_ASSERT_EQUAL(4000, client.battery().mv);
  // the handshake starts with DEVICE_QUERY and sets the clock
  TEST_ASSERT_EQUAL_HEX8(CMD_DEVICE_QUERY, radio.sent[0][0]);
  TEST_ASSERT_EQUAL_HEX8(CMD_APP_START, radio.sent[1][0]);
  TEST_ASSERT_EQUAL_HEX8(CMD_SET_DEVICE_TIME, radio.sent[2][0]);

  // incoming: MSG_WAITING -> SYNC loop until NO_MORE_MESSAGES
  Bytes m1{16, 0, 0, 0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0xFF, 0};
  put32(m1, 1750000001); m1.push_back('A');
  Bytes m2{17, 0, 0, 0, 0, 1, 0};
  put32(m2, 1750000002);
  const char* t = "Bob: B"; m2.insert(m2.end(), t, t + strlen(t));
  radio.queued.push_back(m1);
  radio.queued.push_back(m2);
  radio.pendingPush.push_back({0x83});
  run(client);
  TEST_ASSERT_EQUAL(2, rec.msgs.size());
  TEST_ASSERT_EQUAL_STRING("A", rec.msgs[0].text.c_str());
  TEST_ASSERT_TRUE(rec.msgs[1].isChannel);

  // outgoing DM with ACK -> sent then delivered with trip time
  Contact alice;
  TEST_ASSERT_TRUE(client.findContactByName("ali", alice));
  client.sendDirect(7, alice.pubKey, "hello");
  run(client);
  TEST_ASSERT_TRUE(rec.statuses.size() >= 2);
  TEST_ASSERT_EQUAL(7, rec.statuses.back().first);
  TEST_ASSERT_TRUE(rec.statuses.back().second == MsgStatus::Delivered);
  TEST_ASSERT_EQUAL(850, rec.lastRtt);

  // channel message -> "sent" (no ACK on channels)
  client.sendChannel(8, 0, "to all");
  run(client);
  TEST_ASSERT_EQUAL(8, rec.statuses.back().first);
  TEST_ASSERT_TRUE(rec.statuses.back().second == MsgStatus::Sent);

  // login push opens the request gate
  Contact room;
  TEST_ASSERT_TRUE(client.findContactByName("Room", room));
  client.login(room.pubKey, "pw");
  run(client);
  TEST_ASSERT_EQUAL(1, rec.logins);
}

void test_client_retries_then_fails() {
  gNow = 1000;
  FakeRadio radio;
  radio.ackMessages = false;
  Recorder rec;
  MeshClient client([] { return gNow; }, [] { return (uint32_t)1750000000; });
  client.setListener(&rec);
  client.setTransport(&radio);
  run(client);
  Contact alice;
  TEST_ASSERT_TRUE(client.findContactByName("Alice", alice));
  client.sendDirect(42, alice.pubKey, "anyone?");
  run(client, 200, 100);  // 20 s: 3 attempts of ~3 s each
  int sends = 0, resets = 0;
  for (auto& f : radio.sent) {
    if (f[0] == CMD_SEND_TXT_MSG) {
      TEST_ASSERT_EQUAL(sends, f[2]);  // attempt counter 0,1,2
      sends++;
    }
    if (f[0] == CMD_RESET_PATH) resets++;
  }
  TEST_ASSERT_EQUAL(3, sends);
  TEST_ASSERT_EQUAL(1, resets);  // last attempt floods
  TEST_ASSERT_TRUE(rec.statuses.back().second == MsgStatus::Failed);

  // resend restarts the counter
  radio.ackMessages = true;
  client.sendDirect(42, alice.pubKey, "anyone?");
  run(client);
  TEST_ASSERT_TRUE(rec.statuses.back().second == MsgStatus::Delivered);
}

void test_client_reconnects_and_flushes_queue() {
  gNow = 1000;
  FakeRadio radio;
  Recorder rec;
  MeshClient client([] { return gNow; }, [] { return (uint32_t)0; });  // no NTP: asks module time
  client.setListener(&rec);
  client.setTransport(&radio);
  run(client);
  TEST_ASSERT_TRUE(client.ready());
  bool askedTime = false;
  for (auto& f : radio.sent) if (f[0] == CMD_GET_DEVICE_TIME) askedTime = true;
  TEST_ASSERT_TRUE(askedTime);

  radio.up = false;  // link drops
  run(client, 5);
  TEST_ASSERT_FALSE(client.ready());
  Contact alice;
  client.findContactByName("Alice", alice);
  client.sendDirect(9, alice.pubKey, "queued while offline");
  run(client, 400, 10);  // backoff 2 s then reconnect + handshake
  TEST_ASSERT_TRUE(client.ready());
  TEST_ASSERT_TRUE(rec.statuses.back().first == 9);
  TEST_ASSERT_TRUE(rec.statuses.back().second == MsgStatus::Delivered);

  client.release(0);
  run(client, 10);
  TEST_ASSERT_TRUE(client.state() == LinkState::Released);
  TEST_ASSERT_FALSE(radio.up);
  client.reconnect();
  run(client, 50);
  TEST_ASSERT_TRUE(client.ready());
}

// A channel read with a lost answer must say so: that slot may hold a channel, so it is not "free".
void test_client_channel_read_completeness() {
  gNow = 1000;
  FakeRadio radio;
  Recorder rec;
  MeshClient client([] { return gNow; }, [] { return (uint32_t)1750000000; });
  client.setListener(&rec);
  client.setTransport(&radio);
  run(client);
  TEST_ASSERT_TRUE(client.ready());
  TEST_ASSERT_TRUE(client.channelsComplete());  // error answers for empty slots count as answers

  radio.muteChannel = 0;  // the public channel's answer gets lost
  rec.channelsLoaded = false;
  client.refreshChannels();
  run(client, 400, 100);  // past the command timeout
  TEST_ASSERT_TRUE(rec.channelsLoaded);
  TEST_ASSERT_EQUAL(0, client.channels().size());
  TEST_ASSERT_FALSE(client.channelsComplete());

  radio.muteChannel = -1;
  client.refreshChannels();
  run(client);
  TEST_ASSERT_EQUAL(1, client.channels().size());
  TEST_ASSERT_TRUE(client.channelsComplete());
}

void test_contact_store_and_pushes() {
  gNow = 1000;
  FakeRadio radio;
  Recorder rec;
  MeshClient client([] { return gNow; }, [] { return (uint32_t)1750000000; });
  client.setListener(&rec);
  client.setTransport(&radio);
  run(client);
  TEST_ASSERT_EQUAL(2, client.contactCount());
  // a brand new node is heard
  radio.pendingPush.push_back(contactFrame(0x8A, 0x70, ADV_CHAT, "Newbie"));
  run(client, 5);
  TEST_ASSERT_EQUAL(3, client.contactCount());
  Contact c;
  uint8_t key[32];
  for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x70 + i);
  TEST_ASSERT_TRUE(client.findContact(key, 6, c));
  TEST_ASSERT_EQUAL_STRING("Newbie", c.name);
  TEST_ASSERT_TRUE(client.findContact(key, 32, c));
  // the module deletes Alice (table full, overwrite oldest)
  Bytes del{0x8F};
  for (int i = 0; i < 32; i++) del.push_back((uint8_t)(0x10 + i));
  radio.pendingPush.push_back(del);
  run(client, 5);
  TEST_ASSERT_EQUAL(2, client.contactCount());
  TEST_ASSERT_FALSE(client.findContactByName("Alice", c));
  TEST_ASSERT_TRUE(client.findContactByName("Newbie", c));
  TEST_ASSERT_TRUE(client.findContactByName("Room", c));
  // long route: only CONTACT_PATH_CACHE bytes are kept, pathCached() says so
  Bytes lf = contactFrame(3, 0x90, ADV_CHAT, "Far", 30);  // 30 hops of 1 byte
  TEST_ASSERT_TRUE(dec::contact(lf.data(), lf.size(), c));
  TEST_ASSERT_FALSE(c.pathCached());
  TEST_ASSERT_EQUAL(30, pathHops(c.outPathLen));
  TEST_ASSERT_EQUAL_STRING("Far", c.name);
  TEST_ASSERT_EQUAL(1700000100u, c.lastmod);  // fields after the 64-byte path still decoded
}

void setUp() {}
void tearDown() {}

static void test_wifi_pass() {
  struct N { std::string ssid, pass; };
  std::vector<N> saved = {{"maison", "secret1"}, {"boitier", "secret2"}, {"ouvert", ""}};
  const std::string mask = "********";
  // moved up, moved down: each keeps its own password
  TEST_ASSERT_EQUAL_STRING("secret2", Net::wifiPassFor(saved, "boitier", mask).c_str());
  TEST_ASSERT_EQUAL_STRING("secret1", Net::wifiPassFor(saved, "maison", mask).c_str());
  // a new network never inherits a password
  TEST_ASSERT_EQUAL_STRING("", Net::wifiPassFor(saved, "voisin", mask).c_str());
  TEST_ASSERT_EQUAL_STRING("", Net::wifiPassFor(saved, "ouvert", mask).c_str());
  // what was typed wins, even empty
  TEST_ASSERT_EQUAL_STRING("nouveau", Net::wifiPassFor(saved, "maison", "nouveau").c_str());
  TEST_ASSERT_EQUAL_STRING("", Net::wifiPassFor(saved, "maison", "").c_str());
}

static void test_wifi_pick() {
  using Net::WifiCandidate;
  std::vector<WifiCandidate> c(3);  // home, 4G router, phone
  TEST_ASSERT_EQUAL(-1, Net::pickWifi({}, 0));
  // nothing in sight: blind rotation (hidden networks)
  TEST_ASSERT_EQUAL(0, Net::pickWifi(c, 0));
  TEST_ASSERT_EQUAL(1, Net::pickWifi(c, 1));
  TEST_ASSERT_EQUAL(0, Net::pickWifi(c, 3));
  // away from home with the router on
  c[1].rssi = -60;
  TEST_ASSERT_EQUAL(1, Net::pickWifi(c, 7));
  // both in sight: the default one wins, even with a weaker signal
  c[0].rssi = -85;
  TEST_ASSERT_EQUAL(0, Net::pickWifi(c, 7));
  // the default one just failed (wrong password): go on with the next one
  c[0].failed = true;
  TEST_ASSERT_EQUAL(1, Net::pickWifi(c, 7));
  // all in sight failed: still try rather than stay off
  c[1].failed = true;
  TEST_ASSERT_EQUAL(0, Net::pickWifi(c, 7));

  // on the router: back home only with a usable signal, and not to a network that just failed
  std::vector<WifiCandidate> b(3);
  TEST_ASSERT_EQUAL(-1, Net::betterWifi(b, 1, -80));
  b[0].rssi = -85;
  TEST_ASSERT_EQUAL(-1, Net::betterWifi(b, 1, -80));
  b[0].rssi = -70;
  TEST_ASSERT_EQUAL(0, Net::betterWifi(b, 1, -80));
  b[0].failed = true;
  TEST_ASSERT_EQUAL(-1, Net::betterWifi(b, 1, -80));
  // already on the default one: nothing is better; on an unknown network: any saved one is
  b[0].failed = false;
  TEST_ASSERT_EQUAL(-1, Net::betterWifi(b, 0, -80));
  TEST_ASSERT_EQUAL(0, Net::betterWifi(b, 3, -80));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_device_query);
  RUN_TEST(test_encode_app_start);
  RUN_TEST(test_encode_send_txt);
  RUN_TEST(test_encode_channel_and_set_channel);
  RUN_TEST(test_encode_misc);
  RUN_TEST(test_decode_device_info);
  RUN_TEST(test_decode_self_info);
  RUN_TEST(test_decode_contact);
  RUN_TEST(test_decode_dm_v3);
  RUN_TEST(test_decode_room_signed);
  RUN_TEST(test_decode_channel_v3_and_legacy);
  RUN_TEST(test_decode_sent_confirmed_login);
  RUN_TEST(test_decode_status_trace_batt_stats);
  RUN_TEST(test_decode_path_discovery);
  RUN_TEST(test_hashtag_secret);
  RUN_TEST(test_utf8_truncate_and_hex);
  RUN_TEST(test_tcp_framer);
  RUN_TEST(test_client_handshake_and_messages);
  RUN_TEST(test_client_retries_then_fails);
  RUN_TEST(test_client_reconnects_and_flushes_queue);
  RUN_TEST(test_contact_store_and_pushes);
  RUN_TEST(test_client_channel_read_completeness);
  RUN_TEST(test_wifi_pick);
  RUN_TEST(test_wifi_pass);
  RUN_TEST(test_route_parse);
  RUN_TEST(test_route_names);
  RUN_TEST(test_route_fit);
  RUN_TEST(test_route_several);
  RUN_TEST(test_route_command);
  return UNITY_END();
}
