#include "SendCommands.hh"

#include <inttypes.h>
#include <string.h>
#include <event2/buffer.h>

#include <memory>
#include <phosg/Encoding.hh>
#include <phosg/Random.hh>
#include <phosg/Strings.hh>
#include <phosg/Time.hh>
#include <phosg/Hash.hh>

#include "PSOProtocol.hh"
#include "CommandFormats.hh"
#include "Compression.hh"
#include "FileContentsCache.hh"
#include "Text.hh"

using namespace std;



const unordered_set<uint32_t> v2_crypt_initial_client_commands({
  0x00260088, // (17) DCNTE license check
  0x00B0008B, // (02) DCNTE login
  0x0114008B, // (02) DCNTE extended login
  0x00280090, // (17) DCv1 license check
  0x00B00093, // (02) DCv1 login
  0x01140093, // (02) DCv1 extended login
  0x00E0009A, // (17) DCv2 license check
  0x00CC009D, // (02) DCv2 login
  0x00CC019D, // (02) DCv2 login (UDP off)
  0x0130009D, // (02) DCv2 extended login
  0x0130019D, // (02) DCv2 extended login (UDP off)
  // Note: PSO PC initial commands are not listed here because we don't use a
  // detector encryption for PSO PC (instead, we use the split reconnect command
  // to send PC to a different port).
});
const unordered_set<uint32_t> v3_crypt_initial_client_commands({
  0x00E000DB, // (17) GC/XB license check
  0x00EC009E, // (02) GC login
  0x00EC019E, // (02) GC login (UDP off)
  0x0150009E, // (02) GC extended login
  0x0150019E, // (02) GC extended login (UDP off)
  0x0130009E, // (02) XB login
  0x0130019E, // (02) XB login (UDP off)
  0x0194009E, // (02) XB extended login
  0x0194019E, // (02) XB extended login (UDP off)
});

const unordered_set<string> bb_crypt_initial_client_commands({
  string("\xB4\x00\x93\x00\x00\x00\x00\x00", 8),
  string("\xAC\x00\x93\x00\x00\x00\x00\x00", 8),
  string("\xDC\x00\xDB\x00\x00\x00\x00\x00", 8),
});



void send_command(shared_ptr<Client> c, uint16_t command, uint32_t flag,
    const void* data, size_t size) {
  c->channel.send(command, flag, data, size);
}

void send_command_excluding_client(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint16_t command, uint32_t flag, const void* data, size_t size) {
  for (auto& client : l->clients) {
    if (!client || (client == c)) {
      continue;
    }
    send_command(client, command, flag, data, size);
  }
}

void send_command(shared_ptr<Lobby> l, uint16_t command, uint32_t flag,
    const void* data, size_t size) {
  send_command_excluding_client(l, nullptr, command, flag, data, size);
}

void send_command(shared_ptr<ServerState> s, uint16_t command, uint32_t flag,
    const void* data, size_t size) {
  for (auto& l : s->all_lobbies()) {
    send_command(l, command, flag, data, size);
  }
}

template <typename HeaderT>
void send_command_with_header_t(Channel& ch, const void* data, size_t size) {
  const HeaderT* header = reinterpret_cast<const HeaderT*>(data);
  ch.send(header->command, header->flag, header + 1, size - sizeof(HeaderT));
}

void send_command_with_header(Channel& ch, const void* data, size_t size) {
  switch (ch.version) {
    case GameVersion::DC:
    case GameVersion::GC:
    case GameVersion::XB:
      send_command_with_header_t<PSOCommandHeaderDCV3>(ch, data, size);
      break;
    case GameVersion::PC:
    case GameVersion::PATCH:
      send_command_with_header_t<PSOCommandHeaderPC>(ch, data, size);
      break;
    case GameVersion::BB:
      send_command_with_header_t<PSOCommandHeaderBB>(ch, data, size);
      break;
    default:
      throw logic_error("unimplemented game version in send_command_with_header");
  }
}



static const char* anti_copyright = "This server is in no way affiliated, sponsored, or supported by SEGA Enterprises or SONICTEAM. The preceding message exists only to remain compatible with programs that expect it.";
static const char* dc_port_map_copyright = "DreamCast Port Map. Copyright SEGA Enterprises. 1999";
static const char* dc_lobby_server_copyright = "DreamCast Lobby Server. Copyright SEGA Enterprises. 1999";
static const char* bb_game_server_copyright = "Phantasy Star Online Blue Burst Game Server. Copyright 1999-2004 SONICTEAM.";
static const char* bb_pm_server_copyright = "PSO NEW PM Server. Copyright 1999-2002 SONICTEAM.";
static const char* patch_server_copyright = "Patch Server. Copyright SonicTeam, LTD. 2001";

S_ServerInitWithAfterMessage_DC_PC_V3_02_17_91_9B<0xB4>
prepare_server_init_contents_console(
    uint32_t server_key, uint32_t client_key, uint8_t flags) {
  bool initial_connection = (flags & SendServerInitFlag::IS_INITIAL_CONNECTION);
  S_ServerInitWithAfterMessage_DC_PC_V3_02_17_91_9B<0xB4> cmd;
  cmd.basic_cmd.copyright = initial_connection
      ? dc_port_map_copyright : dc_lobby_server_copyright;
  cmd.basic_cmd.server_key = server_key;
  cmd.basic_cmd.client_key = client_key;
  cmd.after_message = anti_copyright;
  return cmd;
}

void send_server_init_dc_pc_v3(shared_ptr<Client> c, uint8_t flags) {
  bool initial_connection = (flags & SendServerInitFlag::IS_INITIAL_CONNECTION);
  uint8_t command = initial_connection ? 0x17 : 0x02;
  uint32_t server_key = random_object<uint32_t>();
  uint32_t client_key = random_object<uint32_t>();

  auto cmd = prepare_server_init_contents_console(
      server_key, client_key, initial_connection);
  send_command_t(c, command, 0x00, cmd);

  switch (c->version()) {
    case GameVersion::PC:
      c->channel.crypt_in.reset(new PSOV2Encryption(client_key));
      c->channel.crypt_out.reset(new PSOV2Encryption(server_key));
      break;
    case GameVersion::DC:
    case GameVersion::GC:
    case GameVersion::XB: {
      shared_ptr<PSOV2OrV3DetectorEncryption> det_crypt(new PSOV2OrV3DetectorEncryption(
          client_key, v2_crypt_initial_client_commands, v3_crypt_initial_client_commands));
      c->channel.crypt_in = det_crypt;
      c->channel.crypt_out.reset(new PSOV2OrV3ImitatorEncryption(server_key, det_crypt));
      break;
    }
    default:
      throw invalid_argument("incorrect client version");
  }
}

S_ServerInitWithAfterMessage_BB_03_9B<0xB4>
prepare_server_init_contents_bb(
    const parray<uint8_t, 0x30>& server_key,
    const parray<uint8_t, 0x30>& client_key,
    uint8_t flags) {
  bool use_secondary_message = (flags & SendServerInitFlag::USE_SECONDARY_MESSAGE);
  S_ServerInitWithAfterMessage_BB_03_9B<0xB4> cmd;
  cmd.basic_cmd.copyright = use_secondary_message ? bb_pm_server_copyright : bb_game_server_copyright;
  cmd.basic_cmd.server_key = server_key;
  cmd.basic_cmd.client_key = client_key;
  cmd.after_message = anti_copyright;
  return cmd;
}

void send_server_init_bb(shared_ptr<ServerState> s, shared_ptr<Client> c,
    uint8_t flags) {
  bool use_secondary_message = (flags & SendServerInitFlag::USE_SECONDARY_MESSAGE);
  parray<uint8_t, 0x30> server_key;
  parray<uint8_t, 0x30> client_key;
  random_data(server_key.data(), server_key.bytes());
  random_data(client_key.data(), client_key.bytes());
  auto cmd = prepare_server_init_contents_bb(server_key, client_key, flags);
  send_command_t(c, use_secondary_message ? 0x9B : 0x03, 0x00, cmd);

  static const string primary_expected_first_data("\xB4\x00\x93\x00\x00\x00\x00\x00", 8);
  static const string secondary_expected_first_data("\xDC\x00\xDB\x00\x00\x00\x00\x00", 8);
  shared_ptr<PSOBBMultiKeyDetectorEncryption> detector_crypt(new PSOBBMultiKeyDetectorEncryption(
      s->bb_private_keys,
      bb_crypt_initial_client_commands,
      cmd.basic_cmd.client_key.data(),
      sizeof(cmd.basic_cmd.client_key)));
  c->channel.crypt_in = detector_crypt;
  c->channel.crypt_out.reset(new PSOBBMultiKeyImitatorEncryption(
      detector_crypt, cmd.basic_cmd.server_key.data(),
      sizeof(cmd.basic_cmd.server_key), true));
}

void send_server_init_patch(shared_ptr<Client> c) {
  uint32_t server_key = random_object<uint32_t>();
  uint32_t client_key = random_object<uint32_t>();

  S_ServerInit_Patch_02 cmd;
  cmd.copyright = patch_server_copyright;
  cmd.server_key = server_key;
  cmd.client_key = client_key;
  send_command_t(c, 0x02, 0x00, cmd);

  c->channel.crypt_out.reset(new PSOV2Encryption(server_key));
  c->channel.crypt_in.reset(new PSOV2Encryption(client_key));
}

void send_server_init(
    shared_ptr<ServerState> s, shared_ptr<Client> c, uint8_t flags) {
  switch (c->version()) {
    case GameVersion::DC:
    case GameVersion::PC:
    case GameVersion::GC:
    case GameVersion::XB:
      send_server_init_dc_pc_v3(c, flags);
      break;
    case GameVersion::PATCH:
      send_server_init_patch(c);
      break;
    case GameVersion::BB:
      send_server_init_bb(s, c, flags);
      break;
    default:
      throw logic_error("unimplemented versioned command");
  }
}



void send_update_client_config(shared_ptr<Client> c) {
  S_UpdateClientConfig_DC_PC_V3_04 cmd;
  cmd.player_tag = 0x00010000;
  cmd.guild_card_number = c->license->serial_number;
  cmd.cfg = c->export_config();
  send_command_t(c, 0x04, 0x00, cmd);
}



template <typename CommandT>
void send_quest_open_file_t(
    shared_ptr<Client> c,
    const string& quest_name,
    const string& filename,
    uint32_t file_size,
    QuestFileType type) {
  CommandT cmd;
  uint8_t command_num;
  switch (type) {
    case QuestFileType::ONLINE:
      command_num = 0x44;
      cmd.name = "PSO/" + quest_name;
      cmd.flags = 2;
      break;
    case QuestFileType::GBA_DEMO:
      command_num = 0xA6;
      cmd.name = "GBA Demo";
      cmd.flags = 2;
      break;
    case QuestFileType::DOWNLOAD:
      command_num = 0xA6;
      cmd.name = "PSO/" + quest_name;
      cmd.flags = 0;
      break;
    case QuestFileType::EPISODE_3:
      command_num = 0xA6;
      cmd.name = "PSO/" + quest_name;
      cmd.flags = 3;
      break;
    default:
      throw logic_error("invalid quest file type");
  }
  cmd.unused.clear(0);
  cmd.file_size = file_size;
  cmd.filename = filename.c_str();
  send_command_t(c, command_num, 0x00, cmd);
}

void send_quest_buffer_overflow(
    shared_ptr<ServerState> s, shared_ptr<Client> c) {
  // TODO: Figure out a way to share this state across sessions. Maybe we could
  // e.g. modify send_1D to send a nonzero flag value, which we could use to
  // know that the client already has this patch? Or just add another command in
  // the login sequence?

  // PSO Episode 3 USA doesn't natively support the B2 command, but we can add
  // it back to the game with some tricky commands. For details on how this
  // works, see system/ppc/Episode3USAQuestBufferOverflow.s.
  auto fn = s->function_code_index->name_to_function.at("Episode3USAQuestBufferOverflow");
  if (fn->code.size() > 0x400) {
    throw runtime_error("Episode 3 buffer overflow code must be a single segment");
  }

  static const string filename = "m999999p_e.bin";
  send_quest_open_file_t<S_OpenFile_PC_V3_44_A6>(
      c, "BufferOverflow", filename, 0x18, QuestFileType::EPISODE_3);

  S_WriteFile_13_A7 cmd;
  cmd.filename = filename;
  memcpy(cmd.data, fn->code.data(), fn->code.size());
  if (fn->code.size() < 0x400) {
    memset(&cmd.data[fn->code.size()], 0, 0x400 - fn->code.size());
  }
  cmd.data_size = fn->code.size();
  send_command_t(c, 0xA7, 0x00, cmd);
}

void send_function_call(
    shared_ptr<Client> c,
    shared_ptr<CompiledFunctionCode> code,
    const std::unordered_map<std::string, uint32_t>& label_writes,
    const std::string& suffix,
    uint32_t checksum_addr,
    uint32_t checksum_size) {
  return send_function_call(
      c->channel,
      c->flags,
      code,
      label_writes,
      suffix,
      checksum_addr,
      checksum_size);
}

void send_function_call(
    Channel& ch,
    uint64_t client_flags,
    shared_ptr<CompiledFunctionCode> code,
    const std::unordered_map<std::string, uint32_t>& label_writes,
    const std::string& suffix,
    uint32_t checksum_addr,
    uint32_t checksum_size) {
  if (client_flags & Client::Flag::NO_SEND_FUNCTION_CALL) {
    throw logic_error("client does not support function calls");
  }
  if (code.get() && (client_flags & Client::Flag::SEND_FUNCTION_CALL_CHECKSUM_ONLY)) {
    throw logic_error("client only supports checksums in send_function_call");
  }

  string data;
  uint32_t index = 0;
  if (code.get()) {
    data = code->generate_client_command(label_writes, suffix);
    index = code->index;

    if (client_flags & Client::Flag::ENCRYPTED_SEND_FUNCTION_CALL) {
      uint32_t key = random_object<uint32_t>();

      // This format was probably never used on any little-endian system, but we
      // implement the way it would probably work there if it was used.
      StringWriter w;
      if (code->is_big_endian()) {
        w.put_u32b(data.size());
        w.put_u32b(key);
      } else {
        w.put_u32l(data.size());
        w.put_u32l(key);
      }

      data = prs_compress(data);

      // Round size up to a multiple of 4 for encryption
      data.resize((data.size() + 3) & ~3);
      PSOV2Encryption crypt(key);
      if (code->is_big_endian()) {
        crypt.encrypt_big_endian(data.data(), data.size());
      } else {
        crypt.encrypt(data.data(), data.size());
      }
    }
  }

  S_ExecuteCode_B2 header = {data.size(), checksum_addr, checksum_size};

  StringWriter w;
  w.put(header);
  w.write(data);

  ch.send(0xB2, index, w.str());
}



void send_reconnect(shared_ptr<Client> c, uint32_t address, uint16_t port) {
  S_Reconnect_19 cmd = {{address, port, 0}};
  send_command_t(c, (c->version() == GameVersion::PATCH) ? 0x14 : 0x19, 0x00, cmd);
}

void send_pc_console_split_reconnect(shared_ptr<Client> c, uint32_t address,
    uint16_t pc_port, uint16_t console_port) {
  S_ReconnectSplit_19 cmd;
  cmd.pc_address = address;
  cmd.pc_port = pc_port;
  cmd.gc_command = 0x19;
  cmd.gc_flag = 0x00;
  cmd.gc_size = 0x97;
  cmd.gc_address = address;
  cmd.gc_port = console_port;
  send_command_t(c, 0x19, 0x00, cmd);
}



void send_client_init_bb(shared_ptr<Client> c, uint32_t error) {
  S_ClientInit_BB_00E6 cmd;
  cmd.error = error;
  cmd.player_tag = 0x00010000;
  cmd.guild_card_number = c->license->serial_number;
  cmd.team_id = static_cast<uint32_t>(random_object<uint32_t>());
  cmd.cfg = c->export_config_bb();
  cmd.caps = 0x00000102;
  send_command_t(c, 0x00E6, 0x00000000, cmd);
}

void send_team_and_key_config_bb(shared_ptr<Client> c) {
  send_command_t(c, 0x00E2, 0x00000000, c->game_data.account()->key_config);
}

void send_player_preview_bb(shared_ptr<Client> c, uint8_t player_index,
    const PlayerDispDataBBPreview* preview) {

  if (!preview) {
    // no player exists
    S_PlayerPreview_NoPlayer_BB_00E4 cmd = {player_index, 0x00000002};
    send_command_t(c, 0x00E4, 0x00000000, cmd);

  } else {
    SC_PlayerPreview_CreateCharacter_BB_00E5 cmd = {player_index, *preview};
    send_command_t(c, 0x00E5, 0x00000000, cmd);
  }
}

void send_guild_card_header_bb(shared_ptr<Client> c) {
  uint32_t checksum = c->game_data.account()->guild_cards.checksum();
  S_GuildCardHeader_BB_01DC cmd = {1, sizeof(GuildCardFileBB), checksum};
  send_command_t(c, 0x01DC, 0x00000000, cmd);
}

void send_guild_card_chunk_bb(shared_ptr<Client> c, size_t chunk_index) {
  size_t chunk_offset = chunk_index * 0x6800;
  if (chunk_offset >= sizeof(GuildCardFileBB)) {
    throw logic_error("attempted to send chunk beyond end of guild card file");
  }

  S_GuildCardFileChunk_02DC cmd;

  size_t data_size = min<size_t>(
      sizeof(GuildCardFileBB) - chunk_offset, sizeof(cmd.data));

  cmd.unknown = 0;
  cmd.chunk_index = chunk_index;
  memcpy(
      cmd.data,
      reinterpret_cast<const uint8_t*>(&c->game_data.account()->guild_cards) + chunk_offset,
      data_size);

  send_command(c, 0x02DC, 0x00000000, &cmd, sizeof(cmd) - sizeof(cmd.data) + data_size);
}

static const vector<string> stream_file_entries = {
  "ItemMagEdit.prs",
  "ItemPMT.prs",
  "BattleParamEntry.dat",
  "BattleParamEntry_on.dat",
  "BattleParamEntry_lab.dat",
  "BattleParamEntry_lab_on.dat",
  "BattleParamEntry_ep4.dat",
  "BattleParamEntry_ep4_on.dat",
  "PlyLevelTbl.prs",
};
static FileContentsCache bb_stream_files_cache(3600000000ULL);

void send_stream_file_index_bb(shared_ptr<Client> c) {

  struct S_StreamFileIndexEntry_BB_01EB {
    le_uint32_t size;
    le_uint32_t checksum; // crc32 of file data
    le_uint32_t offset; // offset in stream (== sum of all previous files' sizes)
    ptext<char, 0x40> filename;
  };

  vector<S_StreamFileIndexEntry_BB_01EB> entries;
  size_t offset = 0;
  for (const string& filename : stream_file_entries) {
    string key = "system/blueburst/" + filename;
    auto cache_res = bb_stream_files_cache.get_or_load(key);
    auto& e = entries.emplace_back();
    e.size = cache_res.file->data.size();
    // Computing the checksum can be slow, so we cache it along with the file
    // data. If the cache result was just populated, then it may be different,
    // so we always recompute the checksum in that case.
    if (cache_res.generate_called) {
      e.checksum = crc32(cache_res.file->data.data(), e.size);
      bb_stream_files_cache.replace_obj<uint32_t>(key + ".crc32", e.checksum);
    } else {
      auto compute_checksum = [&](const string&) -> uint32_t {
        return crc32(cache_res.file->data.data(), e.size);
      };
      e.checksum = bb_stream_files_cache.get_obj<uint32_t>(key + ".crc32", compute_checksum).obj;
    }
    e.offset = offset;
    e.filename = filename;
    offset += e.size;
  }
  send_command_vt(c, 0x01EB, entries.size(), entries);
}

void send_stream_file_chunk_bb(shared_ptr<Client> c, uint32_t chunk_index) {
  auto cache_result = bb_stream_files_cache.get("<BB stream file>", +[](const string&) -> string {
    size_t bytes = 0;
    for (const auto& name : stream_file_entries) {
      bytes += bb_stream_files_cache.get_or_load("system/blueburst/" + name).file->data.size();
    }

    string ret;
    ret.reserve(bytes);
    for (const auto& name : stream_file_entries) {
      ret += bb_stream_files_cache.get_or_load("system/blueburst/" + name).file->data;
    }
    return ret;
  });
  const auto& contents = cache_result.file->data;

  S_StreamFileChunk_BB_02EB chunk_cmd;
  chunk_cmd.chunk_index = chunk_index;
  size_t offset = sizeof(chunk_cmd.data) * chunk_index;
  if (offset > contents.size()) {
    throw runtime_error("client requested chunk beyond end of stream file");
  }
  size_t bytes = min<size_t>(contents.size() - offset, sizeof(chunk_cmd.data));
  memcpy(chunk_cmd.data, contents.data() + offset, bytes);

  size_t cmd_size = offsetof(S_StreamFileChunk_BB_02EB, data) + bytes;
  cmd_size = (cmd_size + 3) & ~3;
  send_command(c, 0x02EB, 0x00000000, &chunk_cmd, cmd_size);
}

void send_approve_player_choice_bb(shared_ptr<Client> c) {
  S_ApprovePlayerChoice_BB_00E4 cmd = {c->game_data.bb_player_index, 1};
  send_command_t(c, 0x00E4, 0x00000000, cmd);
}

void send_complete_player_bb(shared_ptr<Client> c) {
  send_command_t(c, 0x00E7, 0x00000000, c->game_data.export_player_bb());
}



////////////////////////////////////////////////////////////////////////////////
// patch functions

void send_enter_directory_patch(shared_ptr<Client> c, const string& dir) {
  S_EnterDirectory_Patch_09 cmd = {dir};
  send_command_t(c, 0x09, 0x00, cmd);
}

void send_patch_file(shared_ptr<Client> c, shared_ptr<PatchFileIndex::File> f) {
  S_OpenFile_Patch_06 open_cmd = {0, f->size, f->name};
  send_command_t(c, 0x06, 0x00, open_cmd);

  for (size_t x = 0; x < f->chunk_crcs.size(); x++) {
    // TODO: The use of StringWriter here is... unfortunate. Write a version of
    // Channel::send that takes iovecs or something to avoid these dumb massive
    // string copies.
    StringWriter w;
    auto data = f->load_data();
    size_t chunk_size = min<uint32_t>(f->size - (x * 0x4000), 0x4000);
    S_WriteFileHeader_Patch_07 write_cmd_header = {
        x, f->chunk_crcs[x], chunk_size};
    w.put(write_cmd_header);
    w.write(data->data() + (x * 0x4000), chunk_size);
    while (w.size() & 7) {
      w.put_u8(0);
    }
    send_command(c, 0x07, 0x00, w.str());
  }

  S_CloseCurrentFile_Patch_08 close_cmd = {0};
  send_command_t(c, 0x08, 0x00, close_cmd);
}



////////////////////////////////////////////////////////////////////////////////
// message functions

void send_text(Channel& ch, StringWriter& w, uint16_t command,
    const u16string& text, bool should_add_color) {
  if ((ch.version == GameVersion::DC) ||
      (ch.version == GameVersion::GC) ||
      (ch.version == GameVersion::XB)) {
    string data = encode_sjis(text);
    if (should_add_color) {
      add_color(w, data.c_str(), data.size());
    } else {
      w.write(data);
    }
    w.put_u8(0);
  } else {
    if (should_add_color) {
      add_color(w, text.c_str(), text.size());
    } else {
      w.write(text.data(), text.size() * sizeof(char16_t));
    }
    w.put_u16(0);
  }
  while (w.str().size() & 3) {
    w.put_u8(0);
  }
  ch.send(command, 0x00, w.str());
}

void send_text(Channel& ch, uint16_t command, const u16string& text, bool should_add_color) {
  StringWriter w;
  send_text(ch, w, command, text, should_add_color);
}

void send_header_text(Channel& ch, uint16_t command,
    uint32_t guild_card_number, const u16string& text, bool should_add_color) {
  StringWriter w;
  w.put(SC_TextHeader_01_06_11_B0_EE({0, guild_card_number}));
  send_text(ch, w, command, text, should_add_color);
}

void send_message_box(shared_ptr<Client> c, const u16string& text) {
  uint16_t command;
  switch (c->version()) {
    case GameVersion::PATCH:
      command = 0x13;
      break;
    case GameVersion::DC:
    case GameVersion::PC:
      command = 0x1A;
      break;
    case GameVersion::GC:
    case GameVersion::XB:
    case GameVersion::BB:
      command = 0xD5;
      break;
    default:
      throw logic_error("invalid game version");
  }
  send_text(c->channel, command, text, true);
}

void send_lobby_name(shared_ptr<Client> c, const u16string& text) {
  send_text(c->channel, 0x8A, text, false);
}

void send_quest_info(shared_ptr<Client> c, const u16string& text,
    bool is_download_quest) {
  send_text(c->channel, is_download_quest ? 0xA5 : 0xA3, text, true);
}

void send_lobby_message_box(shared_ptr<Client> c, const u16string& text) {
  send_header_text(c->channel, 0x01, 0, text, true);
}

void send_ship_info(shared_ptr<Client> c, const u16string& text) {
  send_header_text(c->channel, 0x11, 0, text, true);
}

void send_ship_info(Channel& ch, const u16string& text) {
  send_header_text(ch, 0x11, 0, text, true);
}

void send_text_message(Channel& ch, const std::u16string& text) {
  send_header_text(ch, 0xB0, 0, text, true);
}

void send_text_message(shared_ptr<Client> c, const u16string& text) {
  send_header_text(c->channel, 0xB0, 0, text, true);
}

void send_text_message(shared_ptr<Lobby> l, const u16string& text) {
  for (size_t x = 0; x < l->max_clients; x++) {
    if (l->clients[x]) {
      send_text_message(l->clients[x], text);
    }
  }
}

void send_text_message(shared_ptr<ServerState> s, const u16string& text) {
  // TODO: We should have a collection of all clients (even those not in any
  // lobby) and use that instead here
  for (auto& l : s->all_lobbies()) {
    send_text_message(l, text);
  }
}

void send_chat_message(Channel& ch, const u16string& text) {
  send_header_text(ch, 0x06, 0, text, false);
}

void send_chat_message(shared_ptr<Client> c, uint32_t from_guild_card_number,
    const u16string& from_name, const u16string& text) {
  u16string data;
  if (c->version() == GameVersion::BB) {
    data.append(u"\x09J");
  }
  data.append(remove_language_marker(from_name));
  data.append(u"\x09\x09J");
  data.append(text);
  send_header_text(c->channel, 0x06, from_guild_card_number, data, false);
}

template <typename CmdT>
void send_simple_mail_t(
    shared_ptr<Client> c,
    uint32_t from_guild_card_number,
    const u16string& from_name,
    const u16string& text) {
  CmdT cmd;
  cmd.player_tag = 0x00010000;
  cmd.from_guild_card_number = from_guild_card_number;
  cmd.from_name = from_name;
  cmd.to_guild_card_number = c->license->serial_number;
  cmd.text = text;
  send_command_t(c, 0x81, 0x00, cmd);
}

void send_simple_mail_bb(
    shared_ptr<Client> c,
    uint32_t from_guild_card_number,
    const u16string& from_name,
    const u16string& text) {
  SC_SimpleMail_BB_81 cmd;
  cmd.player_tag = 0x00010000;
  cmd.from_guild_card_number = from_guild_card_number;
  cmd.from_name = from_name;
  cmd.to_guild_card_number = c->license->serial_number;
  cmd.received_date = decode_sjis(format_time(now()));
  cmd.text = text;
  send_command_t(c, 0x81, 0x00, cmd);
}

void send_simple_mail(shared_ptr<Client> c, uint32_t from_guild_card_number,
    const u16string& from_name, const u16string& text) {
  switch (c->version()) {
    case GameVersion::DC:
    case GameVersion::GC:
    case GameVersion::XB:
      send_simple_mail_t<SC_SimpleMail_DC_V3_81>(
          c, from_guild_card_number, from_name, text);
      break;
    case GameVersion::PC:
      send_simple_mail_t<SC_SimpleMail_PC_81>(
          c, from_guild_card_number, from_name, text);
      break;
    case GameVersion::BB:
      send_simple_mail_bb(c, from_guild_card_number, from_name, text);
      break;
    default:
      throw logic_error("unimplemented versioned command");
  }
}



////////////////////////////////////////////////////////////////////////////////
// info board

template <typename CharT>
void send_info_board_t(shared_ptr<Client> c, shared_ptr<Lobby> l) {
  vector<S_InfoBoardEntry_D8<CharT>> entries;
  for (const auto& c : l->clients) {
    if (!c.get()) {
      continue;
    }
    auto& e = entries.emplace_back();
    e.name = c->game_data.player()->disp.name;
    e.message = c->game_data.player()->info_board;
    add_color_inplace(e.message);
  }
  send_command_vt(c, 0xD8, entries.size(), entries);
}

void send_info_board(shared_ptr<Client> c, shared_ptr<Lobby> l) {
  if (c->version() == GameVersion::PC ||
      c->version() == GameVersion::PATCH ||
      c->version() == GameVersion::BB) {
    send_info_board_t<char16_t>(c, l);
  } else {
    send_info_board_t<char>(c, l);
  }
}



template <typename CommandHeaderT, typename CharT>
void send_card_search_result_t(
    shared_ptr<ServerState> s,
    shared_ptr<Client> c,
    shared_ptr<Client> result,
    shared_ptr<Lobby> result_lobby) {
  static const vector<string> version_to_port_name({
      "bb-lobby", "console-lobby", "pc-lobby", "console-lobby", "console-lobby", "bb-lobby"});
  const auto& port_name = version_to_port_name.at(static_cast<size_t>(c->version()));

  S_GuildCardSearchResult<CommandHeaderT, CharT> cmd;
  cmd.player_tag = 0x00010000;
  cmd.searcher_guild_card_number = c->license->serial_number;
  cmd.result_guild_card_number = result->license->serial_number;
  cmd.reconnect_command_header.size = sizeof(cmd.reconnect_command_header) + sizeof(cmd.reconnect_command);
  cmd.reconnect_command_header.command = 0x19;
  cmd.reconnect_command_header.flag = 0x00;
  cmd.reconnect_command.address = s->connect_address_for_client(c);
  cmd.reconnect_command.port = s->name_to_port_config.at(port_name)->port;
  cmd.reconnect_command.unused = 0;

  auto encoded_server_name = encode_sjis(s->name);
  string location_string;
  if (result_lobby->is_game()) {
    string encoded_lobby_name = encode_sjis(result_lobby->name);
    location_string = string_printf("%s,BLOCK01,%s",
        encoded_lobby_name.c_str(), encoded_server_name.c_str());
  } else if (result_lobby->flags & Lobby::Flag::EPISODE_3_ONLY) {
    location_string = string_printf("BLOCK01-C%02" PRIu32 ",BLOCK01,%s",
        result_lobby->lobby_id - 15, encoded_server_name.c_str());
  } else {
    location_string = string_printf("BLOCK01-%02" PRIu32 ",BLOCK01,%s",
        result_lobby->lobby_id, encoded_server_name.c_str());
  }
  cmd.location_string = location_string;
  cmd.extension.menu_id = MenuID::LOBBY;
  cmd.extension.lobby_id = result->lobby_id;
  cmd.extension.player_name = result->game_data.player()->disp.name;

  send_command_t(c, 0x41, 0x00, cmd);
}

void send_card_search_result(
    shared_ptr<ServerState> s,
    shared_ptr<Client> c,
    shared_ptr<Client> result,
    shared_ptr<Lobby> result_lobby) {
  if ((c->version() == GameVersion::DC) ||
      (c->version() == GameVersion::GC) ||
      (c->version() == GameVersion::XB)) {
    send_card_search_result_t<PSOCommandHeaderDCV3, char>(
        s, c, result, result_lobby);
  } else if (c->version() == GameVersion::PC) {
    send_card_search_result_t<PSOCommandHeaderPC, char16_t>(
        s, c, result, result_lobby);
  } else if (c->version() == GameVersion::BB) {
    send_card_search_result_t<PSOCommandHeaderBB, char16_t>(
        s, c, result, result_lobby);
  } else {
    throw logic_error("unimplemented versioned command");
  }
}



template <typename CmdT>
void send_guild_card_dc_pc_v3_t(
    Channel& ch,
    uint32_t guild_card_number,
    const u16string& name,
    const u16string& description,
    uint8_t section_id,
    uint8_t char_class) {
  CmdT cmd;
  cmd.header.subcommand = 0x06;
  cmd.header.size = sizeof(CmdT) / 4;
  cmd.header.unused = 0x0000;
  cmd.player_tag = 0x00010000;
  cmd.guild_card_number = guild_card_number;
  cmd.name = name;
  remove_language_marker_inplace(cmd.name);
  cmd.description = description;
  cmd.present = 1;
  cmd.present2 = 1;
  cmd.section_id = section_id;
  cmd.char_class = char_class;
  ch.send(0x60, 0x00, &cmd, sizeof(cmd));
}

static void send_guild_card_bb(
    Channel& ch,
    uint32_t guild_card_number,
    const u16string& name,
    const u16string& team_name,
    const u16string& description,
    uint8_t section_id,
    uint8_t char_class) {
  G_SendGuildCard_BB_6x06 cmd;
  cmd.header.subcommand = 0x06;
  cmd.header.size = sizeof(cmd) / 4;
  cmd.header.unused = 0x0000;
  cmd.guild_card_number = guild_card_number;
  cmd.name = remove_language_marker(name);
  cmd.team_name = remove_language_marker(team_name);
  cmd.description = description;
  cmd.present = 1;
  cmd.present2 = 1;
  cmd.section_id = section_id;
  cmd.char_class = char_class;
  ch.send(0x60, 0x00, &cmd, sizeof(cmd));
}

void send_guild_card(
    Channel& ch,
    uint32_t guild_card_number,
    const u16string& name,
    const u16string& team_name,
    const u16string& description,
    uint8_t section_id,
    uint8_t char_class) {
  if (ch.version == GameVersion::DC) {
    send_guild_card_dc_pc_v3_t<G_SendGuildCard_DC_6x06>(
        ch, guild_card_number, name, description, section_id, char_class);
  } else if (ch.version == GameVersion::PC) {
    send_guild_card_dc_pc_v3_t<G_SendGuildCard_PC_6x06>(
        ch, guild_card_number, name, description, section_id, char_class);
  } else if ((ch.version == GameVersion::GC) ||
             (ch.version == GameVersion::XB)) {
    send_guild_card_dc_pc_v3_t<G_SendGuildCard_V3_6x06>(
        ch, guild_card_number, name, description, section_id, char_class);
  } else if (ch.version == GameVersion::BB) {
    send_guild_card_bb(
        ch, guild_card_number, name, team_name, description, section_id, char_class);
  } else {
    throw logic_error("unimplemented versioned command");
  }
}

void send_guild_card(shared_ptr<Client> c, shared_ptr<Client> source) {
  if (!source->license) {
    throw runtime_error("source player does not have a license");
  }

  uint32_t guild_card_number = source->license->serial_number;
  u16string name = source->game_data.player()->disp.name;
  u16string description = source->game_data.player()->guild_card_description;
  uint8_t section_id = source->game_data.player()->disp.section_id;
  uint8_t char_class = source->game_data.player()->disp.char_class;

  send_guild_card(
      c->channel, guild_card_number, name, u"", description, section_id, char_class);
}



////////////////////////////////////////////////////////////////////////////////
// menus

template <typename EntryT>
void send_menu_t(
    shared_ptr<Client> c,
    const u16string& menu_name,
    uint32_t menu_id,
    const vector<MenuItem>& items,
    bool is_info_menu) {

  vector<EntryT> entries;
  {
    auto& e = entries.emplace_back();
    e.menu_id = menu_id;
    e.item_id = 0xFFFFFFFF;
    e.flags = 0x0004;
    e.text = menu_name;
  }

  for (const auto& item : items) {
    bool is_visible = true;
    switch (c->version()) {
      case GameVersion::DC:
        is_visible &= !(item.flags & MenuItem::Flag::INVISIBLE_ON_DC);
        if (c->flags & Client::Flag::IS_TRIAL_EDITION) {
          is_visible &= !(item.flags & MenuItem::Flag::INVISIBLE_ON_DCNTE);
        }
        break;
      case GameVersion::PC:
        is_visible &= !(item.flags & MenuItem::Flag::INVISIBLE_ON_PC);
        break;
      case GameVersion::GC:
        is_visible &= !(item.flags & MenuItem::Flag::INVISIBLE_ON_GC);
        if (c->flags & Client::Flag::IS_TRIAL_EDITION) {
          is_visible &= !(item.flags & MenuItem::Flag::INVISIBLE_ON_GC_TRIAL_EDITION);
        }
        break;
      case GameVersion::XB:
        is_visible &= !(item.flags & MenuItem::Flag::INVISIBLE_ON_XB);
        break;
      case GameVersion::BB:
        is_visible &= !(item.flags & MenuItem::Flag::INVISIBLE_ON_BB);
        break;
      default:
        throw runtime_error("menus not supported for this game version");
    }
    if (item.flags & MenuItem::Flag::REQUIRES_MESSAGE_BOXES) {
      is_visible &= !(c->flags & Client::Flag::NO_D6);
    }
    if (item.flags & MenuItem::Flag::REQUIRES_SEND_FUNCTION_CALL) {
      is_visible &= !(c->flags & Client::Flag::NO_SEND_FUNCTION_CALL);
    }
    if (item.flags & MenuItem::Flag::REQUIRES_SAVE_DISABLED) {
      is_visible &= !(c->flags & Client::Flag::SAVE_ENABLED);
    }

    if (is_visible) {
      auto& e = entries.emplace_back();
      e.menu_id = menu_id;
      e.item_id = item.item_id;
      e.flags = (c->version() == GameVersion::BB) ? 0x0004 : 0x0F04;
      e.text = item.name;
    }
  }

  send_command_vt(c, is_info_menu ? 0x1F : 0x07, entries.size() - 1, entries);
}

void send_menu(shared_ptr<Client> c, const u16string& menu_name,
    uint32_t menu_id, const vector<MenuItem>& items, bool is_info_menu) {
  if (c->version() == GameVersion::PC ||
      c->version() == GameVersion::PATCH ||
      c->version() == GameVersion::BB) {
    send_menu_t<S_MenuEntry_PC_BB_07_1F>(c, menu_name, menu_id, items, is_info_menu);
  } else {
    send_menu_t<S_MenuEntry_DC_V3_07_1F>(c, menu_name, menu_id, items, is_info_menu);
  }
}



template <typename CharT>
void send_game_menu_t(shared_ptr<Client> c, shared_ptr<ServerState> s) {
  vector<S_GameMenuEntry<CharT>> entries;
  {
    auto& e = entries.emplace_back();
    e.menu_id = MenuID::GAME;
    e.game_id = 0x00000000;
    e.difficulty_tag = 0x00;
    e.num_players = 0x00;
    e.name = s->name;
    e.episode = 0x00;
    e.flags = 0x04;
  }
  for (shared_ptr<Lobby> l : s->all_lobbies()) {
    if (!l->is_game() || (l->version != c->version())) {
      continue;
    }
    bool l_is_ep3 = !!(l->flags & Lobby::Flag::EPISODE_3_ONLY);
    bool c_is_ep3 = !!(c->flags & Client::Flag::IS_EPISODE_3);
    if (l_is_ep3 != c_is_ep3) {
      continue;
    }
    if ((c->flags & Client::Flag::IS_DC_V1) && (l->flags & Lobby::Flag::NON_V1_ONLY)) {
      continue;
    }

    auto& e = entries.emplace_back();
    e.menu_id = MenuID::GAME;
    e.game_id = l->lobby_id;
    e.difficulty_tag = (l_is_ep3 ? 0x0A : (l->difficulty + 0x22));
    e.num_players = l->count_clients();
    if (c->version() == GameVersion::DC) {
      e.episode = (l->flags & Lobby::Flag::NON_V1_ONLY) ? 1 : 0;
    } else {
      e.episode = ((c->version() == GameVersion::BB) ? (l->max_clients << 4) : 0) | l->episode;
    }
    if (l->flags & Lobby::Flag::EPISODE_3_ONLY) {
      e.flags = (l->password.empty() ? 0 : 2);
    } else {
      e.flags = ((l->episode << 6) | (l->password.empty() ? 0 : 2));
      if (l->flags & Lobby::Flag::BATTLE_MODE) {
        e.flags |= 0x10;
      }
      if (l->flags & Lobby::Flag::CHALLENGE_MODE) {
        e.flags |= 0x20;
      }
      if (l->flags & Lobby::Flag::SOLO_MODE) {
        e.flags |= 0x34;
      }
    }
    e.name = l->name;
  }

  send_command_vt(c, 0x08, entries.size() - 1, entries);
}

void send_game_menu(shared_ptr<Client> c, shared_ptr<ServerState> s) {
  if ((c->version() == GameVersion::DC) ||
      (c->version() == GameVersion::GC) ||
      (c->version() == GameVersion::XB)) {
    send_game_menu_t<char>(c, s);
  } else {
    send_game_menu_t<char16_t>(c, s);
  }
}



template <typename EntryT>
void send_quest_menu_t(
    shared_ptr<Client> c,
    uint32_t menu_id,
    const vector<shared_ptr<const Quest>>& quests,
    bool is_download_menu) {
  vector<EntryT> entries;
  for (const auto& quest : quests) {
    auto& e = entries.emplace_back();
    e.menu_id = menu_id;
    e.item_id = quest->menu_item_id;
    e.name = quest->name;
    e.short_description = quest->short_description;
    add_color_inplace(e.short_description);
  }
  send_command_vt(c, is_download_menu ? 0xA4 : 0xA2, entries.size(), entries);
}

template <typename EntryT>
void send_quest_menu_t(
    shared_ptr<Client> c,
    uint32_t menu_id,
    const vector<MenuItem>& items,
    bool is_download_menu) {
  vector<EntryT> entries;
  for (const auto& item : items) {
    auto& e = entries.emplace_back();
    e.menu_id = menu_id;
    e.item_id = item.item_id;
    e.name = item.name;
    e.short_description = item.description;
    add_color_inplace(e.short_description);
  }
  send_command_vt(c, is_download_menu ? 0xA4 : 0xA2, entries.size(), entries);
}

void send_quest_menu(shared_ptr<Client> c, uint32_t menu_id,
    const vector<shared_ptr<const Quest>>& quests, bool is_download_menu) {
  switch (c->version()) {
    case GameVersion::PC:
      send_quest_menu_t<S_QuestMenuEntry_PC_A2_A4>(c, menu_id, quests, is_download_menu);
      break;
    case GameVersion::DC:
    case GameVersion::GC:
      send_quest_menu_t<S_QuestMenuEntry_DC_GC_A2_A4>(c, menu_id, quests, is_download_menu);
      break;
    case GameVersion::XB:
      send_quest_menu_t<S_QuestMenuEntry_XB_A2_A4>(c, menu_id, quests, is_download_menu);
      break;
    case GameVersion::BB:
      send_quest_menu_t<S_QuestMenuEntry_BB_A2_A4>(c, menu_id, quests, is_download_menu);
      break;
    default:
      throw logic_error("unimplemented versioned command");
  }
}

void send_quest_menu(shared_ptr<Client> c, uint32_t menu_id,
    const vector<MenuItem>& items, bool is_download_menu) {
  switch (c->version()) {
    case GameVersion::PC:
      send_quest_menu_t<S_QuestMenuEntry_PC_A2_A4>(c, menu_id, items, is_download_menu);
      break;
    case GameVersion::DC:
    case GameVersion::GC:
      send_quest_menu_t<S_QuestMenuEntry_DC_GC_A2_A4>(c, menu_id, items, is_download_menu);
      break;
    case GameVersion::XB:
      send_quest_menu_t<S_QuestMenuEntry_XB_A2_A4>(c, menu_id, items, is_download_menu);
      break;
    case GameVersion::BB:
      send_quest_menu_t<S_QuestMenuEntry_BB_A2_A4>(c, menu_id, items, is_download_menu);
      break;
    default:
      throw logic_error("unimplemented versioned command");
  }
}

void send_lobby_list(shared_ptr<Client> c, shared_ptr<ServerState> s) {
  // This command appears to be deprecated, as PSO expects it to be exactly how
  // this server sends it, and does not react if it's different, except by
  // changing the lobby IDs.

  vector<S_LobbyListEntry_83> entries;
  for (shared_ptr<Lobby> l : s->all_lobbies()) {
    if (!(l->flags & Lobby::Flag::DEFAULT)) {
      continue;
    }
    if ((l->flags & Lobby::Flag::NON_V1_ONLY) && (c->flags & Client::Flag::IS_DC_V1)) {
      continue;
    }
    if ((l->flags & Lobby::Flag::EPISODE_3_ONLY) && !(c->flags & Client::Flag::IS_EPISODE_3)) {
      continue;
    }
    auto& e = entries.emplace_back();
    e.menu_id = MenuID::LOBBY;
    e.item_id = l->lobby_id;
    e.unused = 0;
  }

  send_command_vt(c, 0x83, entries.size(), entries);
}



////////////////////////////////////////////////////////////////////////////////
// lobby joining

template <typename LobbyDataT, typename DispDataT>
void send_join_game_t(shared_ptr<Client> c, shared_ptr<Lobby> l) {
  bool is_ep3 = (l->flags & Lobby::Flag::EPISODE_3_ONLY);
  string data(is_ep3 ? sizeof(S_JoinGame_GC_Ep3_64) : sizeof(S_JoinGame<LobbyDataT, DispDataT>), '\0');

  // TODO: This is a terrible way to handle the different Ep3 format within the
  // template. Find a way to make this cleaner.
  auto* cmd = reinterpret_cast<S_JoinGame<LobbyDataT, DispDataT>*>(data.data());
  S_JoinGame_GC_Ep3_64* cmd_ep3 = nullptr;
  if (is_ep3) {
    cmd_ep3 = reinterpret_cast<S_JoinGame_GC_Ep3_64*>(data.data());
    new (cmd_ep3) S_JoinGame_GC_Ep3_64();
  } else {
    new (cmd) S_JoinGame<LobbyDataT, DispDataT>();
  }

  cmd->variations = l->variations;

  size_t player_count = 0;
  for (size_t x = 0; x < 4; x++) {
    if (l->clients[x]) {
      cmd->lobby_data[x].player_tag = 0x00010000;
      cmd->lobby_data[x].guild_card = l->clients[x]->license->serial_number;
      cmd->lobby_data[x].client_id = c->lobby_client_id;
      cmd->lobby_data[x].name = l->clients[x]->game_data.player()->disp.name;
      if (cmd_ep3) {
        cmd_ep3->players_ep3[x].inventory = l->clients[x]->game_data.player()->inventory;
        cmd_ep3->players_ep3[x].disp = convert_player_disp_data<PlayerDispDataDCPCV3>(
            l->clients[x]->game_data.player()->disp);
      }
      player_count++;
    } else {
      cmd->lobby_data[x].clear();
    }
  }

  cmd->client_id = c->lobby_client_id;
  cmd->leader_id = l->leader_id;
  cmd->disable_udp = 0x01; // Unused on PC/XB/BB
  cmd->difficulty = l->difficulty;
  cmd->battle_mode = (l->flags & Lobby::Flag::BATTLE_MODE) ? 1 : 0;
  cmd->event = l->event;
  cmd->section_id = l->section_id;
  cmd->challenge_mode = (l->flags & Lobby::Flag::CHALLENGE_MODE) ? 1 : 0;
  cmd->rare_seed = l->random_seed;
  cmd->episode = l->episode;
  cmd->unused2 = 0x01;
  cmd->solo_mode = (l->flags & Lobby::Flag::SOLO_MODE) ? 1 : 0;
  cmd->unused3 = 0x00;

  send_command(c, 0x64, player_count, data);
}

template <typename LobbyDataT, typename DispDataT>
void send_join_lobby_t(shared_ptr<Client> c, shared_ptr<Lobby> l,
    shared_ptr<Client> joining_client = nullptr) {
  uint8_t command;
  if (l->is_game()) {
    if (joining_client) {
      command = 0x65;
    } else {
      throw logic_error("send_join_lobby_t should not be used for primary game join command");
    }
  } else {
    command = joining_client ? 0x68 : 0x67;
  }

  uint8_t lobby_type = (l->type > 14) ? (l->block - 1) : l->type;
  // Allow non-canonical lobby types on GC. They may work on other versions too,
  // but I haven't verified which values don't crash on each version.
  if (c->version() == GameVersion::GC) {
    if (c->flags & Client::Flag::IS_EPISODE_3) {
      if ((l->type > 0x14) && (l->type < 0xE9)) {
        lobby_type = l->block - 1;
      }
    } else {
      if ((l->type > 0x11) && (l->type != 0x67) && (l->type != 0xD4) && (l->type < 0xFC)) {
        lobby_type = l->block - 1;
      }
    }
  } else {
    if (lobby_type > 0x0E) {
      lobby_type = l->block - 1;
    }
  }

  S_JoinLobby<LobbyDataT, DispDataT> cmd;
  cmd.client_id = c->lobby_client_id;
  cmd.leader_id = l->leader_id;
  cmd.disable_udp = 0x01;
  cmd.lobby_number = lobby_type;
  cmd.block_number = l->block;
  cmd.unknown_a1 = 0;
  cmd.event = l->event;
  cmd.unknown_a2 = 0;
  cmd.unused = 0;

  vector<shared_ptr<Client>> lobby_clients;
  if (joining_client) {
    lobby_clients.emplace_back(joining_client);
  } else {
    for (auto lc : l->clients) {
      if (lc) {
        lobby_clients.emplace_back(lc);
      }
    }
  }

  size_t used_entries = 0;
  for (const auto& lc : lobby_clients) {
    auto& e = cmd.entries[used_entries++];
    e.lobby_data.player_tag = 0x00010000;
    e.lobby_data.guild_card = lc->license->serial_number;
    e.lobby_data.client_id = lc->lobby_client_id;
    e.lobby_data.name = lc->game_data.player()->disp.name;
    e.inventory = lc->game_data.player()->inventory;
    e.disp = convert_player_disp_data<DispDataT>(lc->game_data.player()->disp);
    if ((c->version() == GameVersion::PC) || (c->version() == GameVersion::DC)) {
      e.disp.enforce_v2_limits();
    }
  }

  send_command(c, command, used_entries, &cmd, cmd.size(used_entries));
}

void send_join_lobby(shared_ptr<Client> c, shared_ptr<Lobby> l) {
  if (l->is_game()) {
    switch (c->version()) {
      case GameVersion::PC:
        send_join_game_t<PlayerLobbyDataPC, PlayerDispDataDCPCV3>(c, l);
        break;
      case GameVersion::DC:
      case GameVersion::GC:
        send_join_game_t<PlayerLobbyDataDCGC, PlayerDispDataDCPCV3>(c, l);
        break;
      case GameVersion::XB:
        send_join_game_t<PlayerLobbyDataXB, PlayerDispDataDCPCV3>(c, l);
        break;
      case GameVersion::BB:
        send_join_game_t<PlayerLobbyDataBB, PlayerDispDataBB>(c, l);
        break;
      default:
        throw logic_error("unimplemented versioned command");
    }
  } else {
    switch (c->version()) {
      case GameVersion::PC:
        send_join_lobby_t<PlayerLobbyDataPC, PlayerDispDataDCPCV3>(c, l);
        break;
      case GameVersion::DC:
      case GameVersion::GC:
        send_join_lobby_t<PlayerLobbyDataDCGC, PlayerDispDataDCPCV3>(c, l);
        break;
      case GameVersion::XB:
        send_join_lobby_t<PlayerLobbyDataXB, PlayerDispDataDCPCV3>(c, l);
        break;
      case GameVersion::BB:
        send_join_lobby_t<PlayerLobbyDataBB, PlayerDispDataBB>(c, l);
        break;
      default:
        throw logic_error("unimplemented versioned command");
    }
  }

  // If the client will stop sending message box close confirmations after
  // joining any lobby, set the appropriate flag and update the client config
  if ((c->flags & (Client::Flag::NO_D6_AFTER_LOBBY | Client::Flag::NO_D6)) == Client::Flag::NO_D6_AFTER_LOBBY) {
    c->flags |= Client::Flag::NO_D6;
    send_update_client_config(c);
  }
}

void send_player_join_notification(shared_ptr<Client> c,
    shared_ptr<Lobby> l, shared_ptr<Client> joining_client) {
  switch (c->version()) {
    case GameVersion::PC:
      send_join_lobby_t<PlayerLobbyDataPC, PlayerDispDataDCPCV3>(c, l, joining_client);
      break;
    case GameVersion::DC:
    case GameVersion::GC:
      send_join_lobby_t<PlayerLobbyDataDCGC, PlayerDispDataDCPCV3>(c, l, joining_client);
      break;
    case GameVersion::XB:
      send_join_lobby_t<PlayerLobbyDataXB, PlayerDispDataDCPCV3>(c, l, joining_client);
      break;
    case GameVersion::BB:
      send_join_lobby_t<PlayerLobbyDataBB, PlayerDispDataBB>(c, l, joining_client);
      break;
    default:
      throw logic_error("unimplemented versioned command");
  }
}

void send_player_leave_notification(shared_ptr<Lobby> l, uint8_t leaving_client_id) {
  S_LeaveLobby_66_69_Ep3_E9 cmd = {leaving_client_id, l->leader_id, 1, 0};
  send_command_t(l, l->is_game() ? 0x66 : 0x69, leaving_client_id, cmd);
}

void send_self_leave_notification(shared_ptr<Client> c) {
  S_LeaveLobby_66_69_Ep3_E9 cmd = {c->lobby_client_id, 0, 1, 0};
  send_command_t(c, 0x69, c->lobby_client_id, cmd);
}

void send_get_player_info(shared_ptr<Client> c) {
  if ((c->version() == GameVersion::DC) &&
      (c->flags & Client::Flag::IS_TRIAL_EDITION)) {
    send_command(c, 0x8D, 0x00);
  } else {
    send_command(c, 0x95, 0x00);
  }
}



////////////////////////////////////////////////////////////////////////////////
// Trade window

void send_execute_item_trade(std::shared_ptr<Client> c,
    const std::vector<ItemData>& items) {
  SC_TradeItems_D0_D3 cmd;
  if (items.size() > sizeof(cmd.items) / sizeof(cmd.items[0])) {
    throw logic_error("too many items in execute trade command");
  }
  cmd.target_client_id = c->lobby_client_id;
  cmd.item_count = items.size();
  for (size_t x = 0; x < items.size(); x++) {
    cmd.items[x] = items[x];
  }
  send_command_t(c, 0xD3, 0x00, cmd);
}

void send_execute_card_trade(std::shared_ptr<Client> c,
    const std::vector<std::pair<uint32_t, uint32_t>>& card_to_count) {
  if (!(c->flags & Client::Flag::IS_EPISODE_3)) {
    throw logic_error("cannot send trade cards command to non-Ep3 client");
  }

  SC_TradeCards_GC_Ep3_EE_FlagD0_FlagD3 cmd;
  constexpr size_t max_entries = sizeof(cmd.entries) / sizeof(cmd.entries[0]);
  if (card_to_count.size() > max_entries) {
    throw logic_error("too many items in execute card trade command");
  }

  cmd.target_client_id = c->lobby_client_id;
  cmd.entry_count = card_to_count.size();
  size_t x;
  for (x = 0; x < card_to_count.size(); x++) {
    cmd.entries[x].card_type = card_to_count[x].first;
    cmd.entries[x].count = card_to_count[x].second;
  }
  for (; x < max_entries; x++) {
    cmd.entries[x].card_type = 0;
    cmd.entries[x].count = 0;
  }
  send_command_t(c, 0xEE, 0xD3, cmd);
}



////////////////////////////////////////////////////////////////////////////////
// arrows

void send_arrow_update(shared_ptr<Lobby> l) {
  vector<S_ArrowUpdateEntry_88> entries;

  for (size_t x = 0; x < l->max_clients; x++) {
    if (!l->clients[x]) {
      continue;
    }
    auto& e = entries.emplace_back();
    e.player_tag = 0x00010000;
    e.guild_card_number = l->clients[x]->license->serial_number;
    e.arrow_color = l->clients[x]->lobby_arrow_color;
  }

  for (size_t x = 0; x < l->max_clients; x++) {
    if (!l->clients[x] || (l->clients[x]->flags & Client::Flag::IS_DC_V1)) {
      continue;
    }
    send_command_vt(l->clients[x], 0x88, entries.size(), entries);
  }
}

// tells the player that the joining player is done joining, and the game can resume
void send_resume_game(shared_ptr<Lobby> l, shared_ptr<Client> ready_client) {
  static const be_uint32_t data = 0x72010000;
  send_command_excluding_client(l, ready_client, 0x60, 0x00, &data, sizeof(be_uint32_t));
}



////////////////////////////////////////////////////////////////////////////////
// Game/cheat commands

static vector<G_UpdatePlayerStat_6x9A> generate_stats_change_subcommands(
    uint16_t client_id, PlayerStatsChange stat, uint32_t amount) {
  if (amount > (0x7BF8 * 0xFF) / sizeof(G_UpdatePlayerStat_6x9A)) {
    throw runtime_error("stats change command is too large");
  }

  uint8_t stat_ch = static_cast<uint8_t>(stat);
  vector<G_UpdatePlayerStat_6x9A> subs;
  while (amount > 0) {
    uint8_t sub_amount = min<size_t>(amount, 0xFF);
    subs.emplace_back(G_UpdatePlayerStat_6x9A{{0x9A, 0x02, client_id}, 0, stat_ch, sub_amount});
    amount -= sub_amount;
  }
  return subs;
}

void send_player_stats_change(shared_ptr<Lobby> l, shared_ptr<Client> c,
    PlayerStatsChange stat, uint32_t amount) {
  auto subs = generate_stats_change_subcommands(c->lobby_client_id, stat, amount);
  send_command_vt(l, (subs.size() > 0x400 / sizeof(G_UpdatePlayerStat_6x9A)) ? 0x6C : 0x60, 0x00, subs);
}

void send_player_stats_change(Channel& ch, uint16_t client_id, PlayerStatsChange stat, uint32_t amount) {
  auto subs = generate_stats_change_subcommands(client_id, stat, amount);
  send_command_vt(ch, (subs.size() > 0x400 / sizeof(G_UpdatePlayerStat_6x9A)) ? 0x6C : 0x60, 0x00, subs);
}

void send_warp(Channel& ch, uint8_t client_id, uint32_t area) {
  G_InterLevelWarp_6x94 cmd = {{0x94, 0x02, 0}, area, {}};
  ch.send(0x62, client_id, &cmd, sizeof(cmd));
}

void send_warp(shared_ptr<Client> c, uint32_t area) {
  send_warp(c->channel, c->lobby_client_id, area);
  c->area = area;
}

void send_ep3_change_music(shared_ptr<Client> c, uint32_t song) {
  G_ChangeLobbyMusic_GC_Ep3_6xBF cmd = {{0xBF, 0x02, 0}, song};
  send_command_t(c, 0x60, 0x00, cmd);
}

void send_set_player_visibility(shared_ptr<Lobby> l, shared_ptr<Client> c,
    bool visible) {
  uint8_t subcmd = visible ? 0x23 : 0x22;
  uint16_t client_id = c->lobby_client_id;
  G_SetPlayerVisibility_6x22_6x23 cmd = {{subcmd, 0x01, client_id}};
  send_command_t(l, 0x60, 0x00, cmd);
}



////////////////////////////////////////////////////////////////////////////////
// BB game commands

void send_drop_item(Channel& ch, const ItemData& item,
    bool from_enemy, uint8_t area, float x, float z, uint16_t request_id) {
  G_DropItem_PC_V3_BB_6x5F cmd = {
      {{0x5F, 0x0B, 0x0000}, area, from_enemy, request_id, x, z, 0, 0, item}, 0};
  ch.send(0x60, 0x00, &cmd, sizeof(cmd));
}

void send_drop_item(shared_ptr<Lobby> l, const ItemData& item,
    bool from_enemy, uint8_t area, float x, float z, uint16_t request_id) {
  G_DropItem_PC_V3_BB_6x5F cmd = {
      {{0x5F, 0x0B, 0x0000}, area, from_enemy, request_id, x, z, 0, 0, item}, 0};
  send_command_t(l, 0x60, 0x00, cmd);
}

// Notifies other players that a stack was split and part of it dropped (a new
// item was created)
void send_drop_stacked_item(shared_ptr<Lobby> l, const ItemData& item,
    uint8_t area, float x, float z) {
  // TODO: Is this order correct? The original code sent {item, 0}, but it seems
  // GC sends {0, item} (the last two fields in the struct are switched).
  G_DropStackedItem_PC_V3_BB_6x5D cmd = {
      {{0x5D, 0x0A, 0x0000}, area, 0, x, z, item}, 0};
  send_command_t(l, 0x60, 0x00, cmd);
}

void send_pick_up_item(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint32_t item_id, uint8_t area) {
  uint16_t client_id = c->lobby_client_id;
  G_PickUpItem_6x59 cmd = {
      {0x59, 0x03, client_id}, client_id, area, item_id};
  send_command_t(l, 0x60, 0x00, cmd);
}

// Creates an item in a player's inventory (used for withdrawing items from the
// bank)
void send_create_inventory_item(shared_ptr<Lobby> l, shared_ptr<Client> c,
    const ItemData& item) {
  uint16_t client_id = c->lobby_client_id;
  G_CreateInventoryItem_BB_6xBE cmd = {{0xBE, 0x07, client_id}, item, 0};
  send_command_t(l, 0x60, 0x00, cmd);
}

// destroys an item
void send_destroy_item(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint32_t item_id, uint32_t amount) {
  uint16_t client_id = c->lobby_client_id;
  G_DeleteInventoryItem_6x29 cmd = {{0x29, 0x03, client_id}, item_id, amount};
  send_command_t(l, 0x60, 0x00, cmd);
}

// sends the player their bank data
void send_bank(shared_ptr<Client> c) {
  vector<PlayerBankItem> items(c->game_data.player()->bank.items,
      &c->game_data.player()->bank.items[c->game_data.player()->bank.num_items]);

  uint32_t checksum = random_object<uint32_t>();
  G_BankContentsHeader_BB_6xBC cmd = {
      {{0xBC, 0, 0}, 0}, checksum, c->game_data.player()->bank.num_items, c->game_data.player()->bank.meseta};

  size_t size = 8 + sizeof(cmd) + items.size() * sizeof(PlayerBankItem);
  cmd.header.size = size;

  send_command_t_vt(c, 0x6C, 0x00, cmd, items);
}

// sends the player a shop's contents
void send_shop(shared_ptr<Client> c, uint8_t shop_type) {
  G_ShopContents_BB_6xB6 cmd = {
    {0xB6, 0x2C, 0x037F},
    shop_type,
    static_cast<uint8_t>(c->game_data.shop_contents.size()),
    0,
    {},
  };

  size_t count = c->game_data.shop_contents.size();
  if (count > sizeof(cmd.entries) / sizeof(cmd.entries[0])) {
    throw logic_error("too many items in shop");
  }

  for (size_t x = 0; x < count; x++) {
    cmd.entries[x] = c->game_data.shop_contents[x];
  }

  send_command(c, 0x6C, 0x00, &cmd, sizeof(cmd) - sizeof(cmd.entries[0]) * (20 - count));
}

// notifies players about a level up
void send_level_up(shared_ptr<Lobby> l, shared_ptr<Client> c) {
  PlayerStats stats = c->game_data.player()->disp.stats;

  for (size_t x = 0; x < c->game_data.player()->inventory.num_items; x++) {
    if ((c->game_data.player()->inventory.items[x].flags & 0x08) &&
        (c->game_data.player()->inventory.items[x].data.data1[0] == 0x02)) {
      stats.dfp += (c->game_data.player()->inventory.items[x].data.data1w[2] / 100);
      stats.atp += (c->game_data.player()->inventory.items[x].data.data1w[3] / 50);
      stats.ata += (c->game_data.player()->inventory.items[x].data.data1w[4] / 200);
      stats.mst += (c->game_data.player()->inventory.items[x].data.data1w[5] / 50);
    }
  }

  G_LevelUp_6x30 cmd = {
      {0x30, sizeof(G_LevelUp_6x30) / 4, c->lobby_client_id},
      stats.atp,
      stats.mst,
      stats.evp,
      stats.hp,
      stats.dfp,
      stats.ata,
      c->game_data.player()->disp.level.load(),
      0};
  send_command_t(l, 0x60, 0x00, cmd);
}

// gives a player EXP
void send_give_experience(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint32_t amount) {
  uint16_t client_id = c->lobby_client_id;
  G_GiveExperience_BB_6xBF cmd = {
      {0xBF, sizeof(G_GiveExperience_BB_6xBF) / 4, client_id}, amount};
  send_command_t(l, 0x60, 0x00, cmd);
}



////////////////////////////////////////////////////////////////////////////////
// ep3 only commands

void send_ep3_card_list_update(shared_ptr<ServerState> s, shared_ptr<Client> c) {
  if (!(c->flags & Client::Flag::HAS_EP3_CARD_DEFS)) {
    const auto& data = s->ep3_data_index->get_compressed_card_definitions();

    StringWriter w;
    w.put_u32l(data.size());
    w.write(data);

    send_command(c, 0xB8, 0x00, w.str());

    c->flags |= Client::Flag::HAS_EP3_CARD_DEFS;
    send_update_client_config(c);
  }
}

void send_ep3_media_update(
    std::shared_ptr<Client> c,
    uint32_t type,
    uint32_t which,
    const std::string& compressed_data) {
  StringWriter w;
  w.put<S_UpdateMediaHeader_GC_Ep3_B9>({type, which, compressed_data.size(), 0});
  w.write(compressed_data);
  while (w.size() & 3) {
    w.put_u8(0);
  }
  send_command(c, 0xB9, 0x00, w.str());
}

// sends the client a generic rank
void send_ep3_rank_update(shared_ptr<Client> c) {
  S_RankUpdate_GC_Ep3_B7 cmd = {
      0, "\0\0\0\0\0\0\0\0\0\0\0", 0x00FFFFFF, 0x00FFFFFF, 0xFFFFFFFF};
  send_command_t(c, 0xB7, 0x00, cmd);
}

void send_ep3_map_list(shared_ptr<ServerState> s, shared_ptr<Lobby> l) {
  const auto& data = s->ep3_data_index->get_compressed_map_list();

  StringWriter w;
  uint32_t subcommand_size = (data.size() + sizeof(G_MapList_GC_Ep3_6xB6x40) + 3) & (~3);
  w.put<G_MapList_GC_Ep3_6xB6x40>(
      G_MapList_GC_Ep3_6xB6x40{{{{0xB6, 0, 0}, subcommand_size}, 0x40, {}}, data.size(), 0});
  w.write(data);
  send_command(l, 0x6C, 0x00, w.str());
}

void send_ep3_map_data(shared_ptr<ServerState> s, shared_ptr<Lobby> l, uint32_t map_id) {
  auto entry = s->ep3_data_index->get_map(map_id);
  const auto& compressed = entry->compressed();

  StringWriter w;
  uint32_t subcommand_size = (compressed.size() + sizeof(G_MapData_GC_Ep3_6xB6x41) + 3) & (~3);
  w.put<G_MapData_GC_Ep3_6xB6x41>({{{{0xB6, 0, 0}, subcommand_size}, 0x41, {}}, entry->map.map_number.load(), compressed.size(), 0});
  w.write(compressed);
  send_command(l, 0x6C, 0x00, w.str());
}

void send_ep3_card_battle_table_state(shared_ptr<Lobby> l, uint16_t table_number) {
  S_CardBattleTableState_GC_Ep3_E4 cmd;
  for (size_t z = 0; z < 4; z++) {
    cmd.entries[z].present = 0;
    cmd.entries[z].unknown_a1 = 0;
    cmd.entries[z].guild_card_number = 0;
  }

  set<shared_ptr<Client>> clients;
  for (const auto& c : l->clients) {
    if (!c) {
      continue;
    }
    if (c->card_battle_table_number == table_number) {
      if (c->card_battle_table_seat_number > 3) {
        throw runtime_error("invalid battle table seat number");
      }
      auto& e = cmd.entries[c->card_battle_table_seat_number];
      if (e.present) {
        throw runtime_error("multiple clients in the same battle table seat");
      }
      e.present = 1;
      e.guild_card_number = c->license->serial_number;
      clients.emplace(c);
    }
  }

  for (const auto& c : clients) {
    send_command_t(c, 0xE4, table_number, cmd);
  }
}

void set_mask_for_ep3_game_command(void* vdata, size_t size, uint8_t mask_key) {
  if (size < 8) {
    throw logic_error("Episode 3 game command is too short for masking");
  }

  auto* header = reinterpret_cast<G_CardBattleCommandHeader_GC_Ep3_6xB3_6xB4_6xB5*>(vdata);
  size_t command_bytes = header->basic_header.size * 4;
  if (command_bytes != size) {
    throw runtime_error("command size field does not match actual size");
  }

  // Don't waste time if the existing mask_key is the same as the requested one
  if (header->mask_key == mask_key) {
    return;
  }

  // If header->mask_key isn't zero when we get here, then the command is
  // already masked with a different mask_key, so unmask it first
  if ((mask_key != 0) && (header->mask_key != 0)) {
    set_mask_for_ep3_game_command(vdata, size, 0);
  }

  // Now, exactly one of header->mask_key and mask_key should be nonzero, and we
  // are either directly masking or unmasking the command. Since this operation
  // is symmetric, we don't need to split it into two cases.
  if ((header->mask_key == 0) == (mask_key == 0)) {
    throw logic_error("only one of header->mask_key and mask_key may be nonzero");
  }

  uint8_t* data = reinterpret_cast<uint8_t*>(vdata);
  uint8_t k = (header->mask_key ^ mask_key) + 0x80;
  for (size_t z = 8; z < command_bytes; z++) {
    k = (k * 7) + 3;
    data[z] ^= k;
  }
  header->mask_key = mask_key;
}



void send_quest_file_chunk(
    shared_ptr<Client> c,
    const string& filename,
    size_t chunk_index,
    const void* data,
    size_t size,
    QuestFileType type) {
  if (size > 0x400) {
    throw invalid_argument("quest file chunks must be 1KB or smaller");
  }

  S_WriteFile_13_A7 cmd;
  cmd.filename = filename;
  memcpy(cmd.data, data, size);
  if (size < 0x400) {
    memset(&cmd.data[size], 0, 0x400 - size);
  }
  cmd.data_size = size;

  send_command_t(c, (type == QuestFileType::ONLINE) ? 0x13 : 0xA7, chunk_index, cmd);
}

void send_quest_file(shared_ptr<Client> c, const string& quest_name,
    const string& basename, const string& contents, QuestFileType type) {

  switch (c->version()) {
    case GameVersion::DC:
      send_quest_open_file_t<S_OpenFile_DC_44_A6>(
          c, quest_name, basename, contents.size(), type);
      break;
    case GameVersion::PC:
    case GameVersion::GC:
    case GameVersion::XB:
      send_quest_open_file_t<S_OpenFile_PC_V3_44_A6>(
          c, quest_name, basename, contents.size(), type);
      break;
    case GameVersion::BB:
      send_quest_open_file_t<S_OpenFile_BB_44_A6>(
        c, quest_name, basename, contents.size(), type);
      break;
    default:
      throw logic_error("cannot send quest files to this version of client");
  }

  for (size_t offset = 0; offset < contents.size(); offset += 0x400) {
    size_t chunk_bytes = contents.size() - offset;
    if (chunk_bytes > 0x400) {
      chunk_bytes = 0x400;
    }
    send_quest_file_chunk(c, basename.c_str(), offset / 0x400,
        contents.data() + offset, chunk_bytes, type);
  }
}

void send_server_time(shared_ptr<Client> c) {
  uint64_t t = now();

  time_t t_secs = t / 1000000;
  struct tm t_parsed;
  gmtime_r(&t_secs, &t_parsed);

  string time_str(128, 0);
  size_t len = strftime(time_str.data(), time_str.size(),
      "%Y:%m:%d: %H:%M:%S.000", &t_parsed);
  if (len == 0) {
    throw runtime_error("format_time buffer too short");
  }
  time_str.resize(len);

  send_command(c, 0xB1, 0x00, time_str);
}

void send_change_event(shared_ptr<Client> c, uint8_t new_event) {
  // This command isn't supported on versions before V3, nor on Trial Edition.
  if ((c->version() == GameVersion::DC) ||
      (c->version() == GameVersion::PC) ||
      (c->flags & Client::Flag::IS_TRIAL_EDITION)) {
    return;
  }
  send_command(c, 0xDA, new_event);
}

void send_change_event(shared_ptr<Lobby> l, uint8_t new_event) {
  for (auto& c : l->clients) {
    if (!c) {
      continue;
    }
    send_change_event(c, new_event);
  }
}

void send_change_event(shared_ptr<ServerState> s, uint8_t new_event) {
  // TODO: Create a collection of all clients on the server (including those not
  // in lobbies) and use that here instead
  for (auto& l : s->all_lobbies()) {
    send_change_event(l, new_event);
  }
}
