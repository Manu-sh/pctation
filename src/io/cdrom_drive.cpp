#include <io/cdrom_drive.hpp>

#include <cpu/interrupt.hpp>
#include <util/fs.hpp>
#include <util/log.hpp>

#include <gsl-lite.hpp>

#include <fstream>
#include <utility>

namespace io {

void CdromDrive::insert_disk_file(const fs::path& file_path) {
  auto ext = file_path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".cue")
    m_disk.init_from_cue(file_path.string().c_str());
  else
    m_disk.init_from_bin(file_path.string().c_str());
  m_stat_code.shell_open = false;
}

void CdromDrive::init(cpu::Interrupts* interrupts) {
  m_interrupts = interrupts;
}

void CdromDrive::step() {
  m_reg_status.transmit_busy = false;

  if (!m_irq_fifo.empty()) {
    auto irq_triggered = m_irq_fifo.front() & 0b111;
    auto irq_mask = m_reg_int_enable & 0b111;

    if (irq_triggered & irq_mask)
      m_interrupts->trigger(cpu::IrqType::CDROM);
  }

  constexpr std::array<u8, 12> SYNC_MAGIC = { { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                0xff, 0xff, 0x00 } };

  if (m_stat_code.reading || m_stat_code.playing) {
    if (--m_steps_until_read_sect == 0) {
      m_steps_until_read_sect = READ_SECTOR_DELAY_STEPS;

      CdromTrack::DataType sector_type;
      const auto pos_to_read = CdromPosition::from_lba(m_read_sector);
      m_read_buf = m_disk.read(pos_to_read, sector_type);

      m_read_sector++;

      if (sector_type == CdromTrack::DataType::Invalid)
        return;

      const auto sector_has_data = (sector_type == CdromTrack::DataType::Data);
      const auto sector_has_audio = (sector_type == CdromTrack::DataType::Audio);

      auto sync_match = std::equal(SYNC_MAGIC.begin(), SYNC_MAGIC.end(), m_read_buf.begin());

      if (m_stat_code.playing && sector_has_audio) {  // Reading audio
        if (sync_match)
          LOG_ERROR_CDROM("Sync data found in Audio sector");
      } else if (m_stat_code.reading && sector_has_data) {  // Reading data
        if (!sync_match)
          LOG_ERROR_CDROM("Sync data mismach in Data sector");

        // ack more data
        push_response(SecondInt1, m_stat_code.byte);
      }
    }
  }
}

u8 CdromDrive::read_reg(address addr_rebased) {
  const u8 reg = addr_rebased;
  const u8 reg_index = m_reg_status.index;

  u8 val = 0;

  if (reg == 0) {  // Status Register
    val = m_reg_status.byte;
  } else if (reg == 1) {  // Response FIFO
    if (!m_resp_fifo.empty()) {
      val = m_resp_fifo.front();
      m_resp_fifo.pop_front();

      if (m_resp_fifo.empty())
        m_reg_status.response_fifo_not_empty = false;
    }
  } else if (reg == 2) {  // Data FIFO
    val = read_byte();
  } else if (reg == 3 && (reg_index == 0 || reg_index == 2)) {  // Interrupt Enable Register
    val = m_reg_int_enable;
  } else if (reg == 3 && (reg_index == 1 || reg_index == 3)) {  // Interrupt Flag Register
    val = 0b11100000;                                           // these bits are always set

    if (!m_irq_fifo.empty())
      val |= m_irq_fifo.front() & 0b111;
  } else {
    LOG_ERROR_CDROM("Unknown combination, CDREG{}.{}", reg, reg_index);
  }

  LOG_TRACE_CDROM("CDROM read {} (CDREG{}.{}) val: 0x{:02X} ({:#010b})",
                  get_reg_name(reg, reg_index, true), reg, reg_index, val, val);

  return val;
}

bool CdromDrive::is_data_buf_empty() {
  if (m_data_buf.empty())
    return true;

  const auto sector_size = m_mode.sector_size();
  if (m_data_buffer_index >= sector_size)
    return true;

  return false;
}

void CdromDrive::write_reg(address addr_rebased, u8 val) {
  const u8 reg = addr_rebased;
  const u8 reg_index = m_reg_status.index;

  if (reg == 0) {  // Index Register
    m_reg_status.index = val & 0b11;
    return;                                 // Don't log in this case
  } else if (reg == 1 && reg_index == 0) {  // Command Register
    execute_command(val);
  } else if (reg == 1 && reg_index == 1) {  // Sound Map Data Out
  } else if (reg == 1 && reg_index == 2) {  // Sound Map Coding Info
  } else if (reg == 1 && reg_index == 3) {  // Audio Volume for Right-CD-Out to Right-SPU-Input
  } else if (reg == 2 && reg_index == 0) {  // Parameter FIFO
    Ensures(m_param_fifo.size() < MAX_FIFO_SIZE);

    m_param_fifo.push_back(val);
    m_reg_status.param_fifo_empty = false;
    m_reg_status.param_fifo_write_ready = (m_param_fifo.size() < MAX_FIFO_SIZE);
  } else if (reg == 2 && reg_index == 1) {  // Interrupt Enable Register
    m_reg_int_enable = val;
  } else if (reg == 2 && reg_index == 2) {  // Audio Volume for Left-CD-Out to Left-SPU-Input
  } else if (reg == 2 && reg_index == 3) {  // Audio Volume for Right-CD-Out to Left-SPU-Input
  } else if (reg == 3 && reg_index == 0) {  // Request Register
    if (val & 0x80) {                       // Want data
      if (is_data_buf_empty()) {  // Only update data buffer if everything from it has been read
        m_data_buf = std::move(m_read_buf);
        m_data_buffer_index = 0;
        m_reg_status.data_fifo_not_empty = true;
      }
    } else {  // Clear data buffer
      m_data_buf.clear();
      m_data_buffer_index = 0;
      m_reg_status.data_fifo_not_empty = false;
    }
  } else if (reg == 3 && reg_index == 1) {  // Interrupt Flag Register
    if (val & 0x40) {                       // Reset Parameter FIFO
      m_param_fifo.clear();
      m_reg_status.param_fifo_empty = true;
      m_reg_status.param_fifo_write_ready = true;
    }
    if (!m_irq_fifo.empty())
      m_irq_fifo.pop_front();
  } else if (reg == 3 && reg_index == 2) {  // Audio Volume for Left-CD-Out to Right-SPU-Input
  } else if (reg == 3 && reg_index == 3) {  // Audio Volume Apply Changes
  } else {
    LOG_ERROR_CDROM("Unknown combination, CDREG{}.{} val: {:02X}", reg, reg_index, val);
  }

  LOG_TRACE_CDROM("CDROM write {} (CDREG{}.{}) val: 0x{:02X} ({:#010b})",
                  get_reg_name(reg, reg_index, false), reg, reg_index, val, val);
}

u8 CdromDrive::read_byte() {
  if (is_data_buf_empty()) {
    LOG_WARN_CDROM("Tried to read with an empty buffer");
    return 0;
  }

  // TODO reads out of bounds
  const bool data_only = (m_mode.sector_size() == 0x800);

  u32 data_offset = data_only ? 24 : 12;

  u8 data = m_data_buf[data_offset + m_data_buffer_index];
  ++m_data_buffer_index;

  if (is_data_buf_empty())
    m_reg_status.data_fifo_not_empty = false;

  return data;
}

u32 CdromDrive::read_word() {
  u32 data{};
  data |= read_byte() << 0;
  data |= read_byte() << 8;
  data |= read_byte() << 16;
  data |= read_byte() << 24;
  return data;
}

void CdromDrive::execute_command(u8 cmd) {
  m_irq_fifo.clear();
  m_resp_fifo.clear();

  LOG_DEBUG_CDROM("CDROM command issued: {} ({:02X})", get_cmd_name(cmd), cmd);

  if (!m_param_fifo.empty())
    LOG_DEBUG_CDROM("Parameters: [{:02X}]", fmt::join(m_param_fifo, ", "));

  switch (cmd) {
    case 0x01:  // Getstat
      push_response_stat(FirstInt3);
      break;
    case 0x02: {  // Setloc
      const auto mm = util::bcd_to_dec(get_param());
      const auto ss = util::bcd_to_dec(get_param());
      const auto ff = util::bcd_to_dec(get_param());

      CdromPosition pos(mm, ss, ff);

      m_seek_sector = pos.to_lba();

      push_response_stat(FirstInt3);
      break;
    }

    // 0Dh Setfilter  E file,channel    INT3(stat)
    case 0x0D: /* CdlSetfilter */ {

      const auto file    = util::bcd_to_dec(get_param());
      const auto channel = util::bcd_to_dec(get_param());
      push_response(FirstInt3, { m_stat_code.byte, util::dec_to_bcd(file), util::dec_to_bcd(channel) });
      break;
    }

    /*
      temporaney workaround to ignore wrong CRC:

      https://psx-spx.consoledev.net/cdromdrive/#cdrom-video-cd-commands
       Anyways, the relevant part is that the modified sectors have wrong CRCs
       (which means that the PSX cdrom controller will ignore them, and the
       GetlocP command will keep returning position data from the previous sector).

       correct response: INT3(track,index,mm,ss,sect,amm,ass,asect)

       https://github.com/stenzek/duckstation/blob/master/src/core/cdrom.cpp#L920
    */
    case 0x11 /*GetlocP*/: case 0x03: // Play
      Expects(m_param_fifo.empty());  // we don't handle the parameter
      m_read_sector = m_seek_sector;

      m_stat_code.set_state(CdromReadState::Playing);

      push_response_stat(FirstInt3);
      break;
    case 0x06:  // ReadN
      m_read_sector = m_seek_sector;

      m_stat_code.set_state(CdromReadState::Reading);

      push_response_stat(FirstInt3);
      break;
    case 0x07:  // MotorOn
      m_stat_code.spindle_motor_on = true;

      push_response_stat(FirstInt3);
      push_response_stat(SecondInt2);
      break;
    case 0x08:  // Stop
      m_stat_code.set_state(CdromReadState::Stopped);
      m_stat_code.spindle_motor_on = false;

      push_response_stat(FirstInt3);
      push_response_stat(SecondInt2);
      break;
    case 0x09:  // Pause
      push_response_stat(FirstInt3);

      m_stat_code.set_state(CdromReadState::Stopped);

      push_response_stat(SecondInt2);
      break;
    case 0x0E: {  // Setmode
      push_response_stat(FirstInt3);

      // TODO: bit 4 behaviour
      const auto param = get_param();
      Expects(!(param & 0b10000));
      m_mode.byte = param;
      break;
    }
    case 0x0B:  // Mute
      m_muted = true;

      push_response_stat(FirstInt3);
      break;
    case 0x0C:  // Demute
      m_muted = false;

      push_response_stat(FirstInt3);
      break;
    case 0x0F:                                                     // Getparam
      push_response(FirstInt3, { m_stat_code.byte, 0x00, 0x00 });  // TODO: last arg is the filter
      break;
    case 0x13: {                                  // GetTN
      const auto index = util::dec_to_bcd(0x01);  // TODO
      const auto track_count = util::dec_to_bcd(m_disk.get_track_count());
      push_response(FirstInt3, { m_stat_code.byte, index, track_count });
      break;
    }
    case 0x14: {  // GetTD
      const auto track_number = util::bcd_to_dec(get_param());

      CdromPosition disk_pos{};
      if (track_number == 0) {  // Special meaning: last track (total size)
        disk_pos = m_disk.size();
      } else {  // Start of a track
        disk_pos = m_disk.get_track_start(track_number);
      }

      u8 minutes = util::dec_to_bcd(disk_pos.minutes);
      u8 seconds = util::dec_to_bcd(disk_pos.seconds);

      push_response(FirstInt3, { m_stat_code.byte, minutes, seconds });
      break;
    }
    case 0x15: {  // SeekL
      push_response_stat(FirstInt3);

      m_read_sector = m_seek_sector;
      m_stat_code.set_state(CdromReadState::Seeking);

      push_response_stat(SecondInt2);
      break;
    }
    case 0x19: {  // Test
      const auto subfunction = get_param();

      LOG_DEBUG_CDROM("  CDROM command subfuncion: {:02X}", subfunction);

      switch (subfunction) {
        case 0x20:  // Get CDROM BIOS date/version (yy,mm,dd,ver)
          push_response(
              FirstInt3,
              { 0x94, 0x09, 0x19, 0xC0 });  // Send reponse of PSX (PU-7), 18 Nov 1994, version vC0 (b)
          break;
        default: {
          command_error();
          LOG_ERROR_CDROM("Unhandled Test subfunction {:02X}", subfunction);
          break;
        }
      }
      break;
    }
    case 0x1A: {  // GetID
      const bool has_disk = !m_disk.is_empty();

      if (m_stat_code.shell_open) {
        push_response(ErrorInt5, { 0x11, 0x80 });
      } else if (has_disk) {  // Disk
        push_response(FirstInt3, m_stat_code.byte);
        push_response(SecondInt2, { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' });
      } else {  // No Disk
        push_response(FirstInt3, m_stat_code.byte);
        push_response(ErrorInt5, { 0x08, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
      }
      break;
    }
    case 0x1B:  // ReadS
      m_read_sector = m_seek_sector;

      m_stat_code.set_state(CdromReadState::Reading);

      push_response_stat(FirstInt3);
      break;
    case 0x0A: {  // Init
      push_response_stat(FirstInt3);

      m_stat_code.reset();
      m_stat_code.spindle_motor_on = true;

      m_mode.reset();

      push_response_stat(SecondInt2);
      break;
    }
/*
    case 0x11: {  // CdlGetlocP: https://github.com/mirror/pcsxr/blob/51b2ad7c2bdc83ed6a0f297679233bc4d19de027/libpcsxcore/cdrom.c#L48


      /*
          SetResultSize(8);
          memcpy(&cdr.Result, &cdr.subq, 8);

          if (!cdr.Play && CheckSBI(cdr.Result+5))
                  memset(cdr.Result+2, 0, 6);
          if (!cdr.Play && !cdr.Reading)
                  cdr.Result[1] = 0; // HACK?
          break;



      break;
    }
*/
    default: {
      command_error();
      LOG_ERROR_CDROM("Unhandled CDROM command 0x{:02X}", cmd);
      break;
    }
  }

  if (!m_resp_fifo.empty())
    LOG_DEBUG_CDROM("Response: [{:02X}]", fmt::join(m_resp_fifo, ", "));

  m_param_fifo.clear();

  m_reg_status.transmit_busy = true;
  m_reg_status.param_fifo_empty = true;
  m_reg_status.param_fifo_write_ready = true;
  m_reg_status.adpcm_fifo_empty = false;
}

void CdromDrive::command_error() {
  push_response(ErrorInt5, { 0x11, 0x40 });
}

u8 CdromDrive::get_param() {
  Expects(!m_param_fifo.empty());

  auto param = m_param_fifo.front();
  m_param_fifo.pop_front();

  m_reg_status.param_fifo_empty = m_param_fifo.empty();
  m_reg_status.param_fifo_write_ready = true;

  return param;
}

void CdromDrive::push_response(CdromResponseType type, std::initializer_list<u8> bytes) {
  // First we write the type (INT value) in the Interrupt FIFO
  m_irq_fifo.push_back(type);

  // Then we write the response's data (args) to the Response FIFO
  for (auto response_byte : bytes) {
    if (m_resp_fifo.size() < MAX_FIFO_SIZE) {
      m_resp_fifo.push_back(response_byte);
      m_reg_status.response_fifo_not_empty = true;
    } else
      LOG_WARN_CDROM("CDROM response 0x{:02} lost, FIFO was full", response_byte);
  }
}

void CdromDrive::push_response(CdromResponseType type, u8 byte) {
  push_response(type, { byte });
}

void CdromDrive::push_response_stat(CdromResponseType type) {
  push_response(type, m_stat_code.byte);
}

const char* CdromDrive::get_cmd_name(u8 cmd) {
  const char* cmd_names[] = { "Sync",       "Getstat",   "Setloc",  "Play",     "Forward", "Backward",
                              "ReadN",      "MotorOn",   "Stop",    "Pause",    "Init",    "Mute",
                              "Demute",     "Setfilter", "Setmode", "Getparam", "GetlocL", "GetlocP",
                              "SetSession", "GetTN",     "GetTD",   "SeekL",    "SeekP",   "-",
                              "-",          "Test",      "GetID",   "ReadS",    "Reset",   "GetQ",
                              "ReadTOC",    "VideoCD" };

  if (cmd <= 0x1F)
    return cmd_names[cmd];
  if (0x50 <= cmd && cmd <= 0x57)
    return "Secret";
  return "<unknown>";
}

const char* CdromDrive::get_reg_name(u8 reg, u8 index, bool is_read) {
  // clang-format off
  if (is_read) {
    if (reg == 0) return "Status Register";
    if (reg == 1 && index == 0) return "Command Register";
    if (reg == 1) return "Response FIFO";
    if (reg == 2) return "Data FIFO";
    if (reg == 3 && (index == 0 || index == 2)) return "Interrupt Enable Register";
    if (reg == 3 && (index == 1 || index == 3)) return "Interrupt Flag Register";
  } else {
    if (reg == 0) return "Index Register";
    if (reg == 1 && index == 0) return "Command Register";
    if (reg == 1 && index == 1) return "Sound Map Data Out";
    if (reg == 1 && index == 2) return "Sound Map Coding Info";
    if (reg == 1 && index == 3) return "Audio Volume for Right-CD-Out to Right-SPU-Input";
    if (reg == 2 && index == 0) return "Parameter FIFO";
    if (reg == 2 && index == 1) return "Interrupt Enable Register";
    if (reg == 2 && index == 2) return "Audio Volume for Left-CD-Out to Left-SPU-Input";
    if (reg == 2 && index == 3) return "Audio Volume for Right-CD-Out to Left-SPU-Input";
    if (reg == 3 && index == 0) return "Request Register";
    if (reg == 3 && index == 1) return "Interrupt Flag Register";
    if (reg == 3 && index == 2) return "Audio Volume for Left-CD-Out to Right-SPU-Input";
    if (reg == 3 && index == 3) return "Audio Volume Apply Changes";
  }
  // clang-format on
  return "<unknown>";
}

}  // namespace io
