#include "Arduino.h"
#include "XModem.h"

XModem::XModem(){
  
}
void XModem::onXmodemUpdate(bool(*callback)(uint8_t code, uint8_t value)){
  _onXmodemUpdateHandler = callback;
}
bool XModem::begin(Stream &serial, XModem::ProtocolType type) {
  if (this->_onXmodemUpdateHandler == nullptr)return false;
  _serial = &serial;
  _protocol = type;
  switch(type) {
    case ProtocolType::XMODEM:
      _id_bytes = 1;
      _chksum_bytes = 1;
      _data_bytes = 128;
      _rx_init_byte = NAK;
      break;
    case ProtocolType::CRC_XMODEM:
      _id_bytes = 1;
      _chksum_bytes = 2;
      _data_bytes = 128;
      _rx_init_byte = 'C';
      break;
  }
  retry_limit = 10;
  _signal_retry_delay_ms = 100;
  _allow_nonsequential = false;
  _buffer_packet_reads = true;
  return true;
}

// SETTERS
void XModem::setIdSize(size_t size) {
  _id_bytes = size;
}

void XModem::setChecksumSize(size_t size) {
  _chksum_bytes = size;
}

void XModem::setDataSize(size_t size) {
  _data_bytes = size;
}

void XModem::setSendInitByte(byte b) {
  _rx_init_byte = b;
}

void XModem::setRetryLimit(byte limit) {
  retry_limit = limit;
}

void XModem::setSignalRetryDelay(unsigned long ms) {
  _signal_retry_delay_ms = ms;
}

void XModem::allowNonSequentailBlocks(bool b) {
  _allow_nonsequential = b;
}

void XModem::bufferPacketReads(bool b) {
  _buffer_packet_reads = b;
}

bool XModem::pathAssert(const char * path){// no pot començar amb / not start /

  // Ensure the SPI pinout the SD card is connected to is configured properly

  String build;
  String stringPath(path);
  
  if(stringPath.length() < 3)return false;// might change, no allow 2 char folder
  if(path[0] == '/')return false;

  int pos = stringPath.indexOf('/');
  while (pos > 0 ) {
    build = stringPath.substring(0,pos);
    if (!SD.exists(build.c_str())) {// <------ mkdir
      if (!SD.mkdir(build.c_str())) {
        //Serial.println("F");
        return false;
        }
    }

      pos = stringPath.indexOf('/',pos+1);
  }
  
  if (!SD.exists(path)) {// <------ mkdir
      if (!SD.mkdir(path)) {
        //Serial.println("F");
        return false;
        }
    }

  return true;
}
/**
 * Improved to archieve ymodem like quality knowing the file size
*/
bool XModem::receiveFile(String filePath,unsigned int size, bool binary){
  this->sizeKnown = size;
  this->binary = binary;
  int lastSeparatorIndex = filePath.lastIndexOf('/');
  String tmp = filePath.substring(0, lastSeparatorIndex + 1);
  this->pathAssert(tmp.c_str());
  
  if(!openFiles(filePath.c_str(), true)){
      return false;
  }

  return this->receive();
}

bool XModem::receive() {
  if(!init_rx() || !rx()) {
    //An unrecoverable error occured send cancels to terminate the transaction
    _serial->write(CAN);
    _serial->write(CAN);
    _serial->write(CAN);
    return false;
  }
  return true;
}

bool XModem::lookup_send(unsigned long long id) {
  return send((uint8_t*) NULL, 0, id);
}
bool XModem::sendFile(String filePath){
  if(!openFiles(filePath.c_str(), false)){
    this->_onXmodemUpdateHandler(2,1);/** SD card problem */
    return false;
  }
  if(!workingFile){
    this->_onXmodemUpdateHandler(1,2);/** file not found*/
    return false;
  }else{
    this->sizeKnown = workingFile.size();
  }
  uint8_t *id = (uint8_t *) malloc(_id_bytes);
  //convert the start_id to big endian format
  unsigned long long temp = 1;
  for(size_t i = 0; i < _id_bytes; ++i) {
    id[_id_bytes-i-1] = (uint8_t) (temp & 0xFF);
    temp >>=8;
  }

  struct bulk_data container;
  /*container.data_arr = &data;
  container.len_arr = &data_len;*/
  container.id_arr = id;
  container.count = 1;

  bool result = send_bulk_data(container,true);
  free(id);
  closeFiles(true);

  return result;
}
bool XModem::send(uint8_t *data, size_t data_len, unsigned long start_id) {
  uint8_t *id = (uint8_t *) malloc(_id_bytes);

  //convert the start_id to big endian format
  unsigned long long temp = start_id;
  for(size_t i = 0; i < _id_bytes; ++i) {
    id[_id_bytes-i-1] = (uint8_t) (temp & 0xFF);
    temp >>=8;
  }

  struct bulk_data container;
  container.data_arr = &data;
  container.len_arr = &data_len;
  container.id_arr = id;
  container.count = 1;

  bool result = send_bulk_data(container);
  free(id);
  return result;
}

bool XModem::send_bulk_data(struct bulk_data container,bool file) {
  if(container.count == 0) return false;

  struct packet p;

  //bundle all our memory allocations together
  //need to store:
  //2 id blocks - blk_id and packet struct
  //1 checksum block - packet struct
  //1 data block - packet struct
  byte *buffer = (byte *) malloc(2*_id_bytes + 1*_chksum_bytes + 1*_data_bytes);
  byte *blk_id = buffer + _data_bytes;
  p.id = blk_id + _id_bytes;
  p.chksum = p.id + _id_bytes;
  p.data = buffer;

  bool result = init_tx();
  if(file){
    for(size_t j = 0; result && j < container.count; ++j) {// packet sending
      for(size_t i = 0; i < _id_bytes; ++i) blk_id[i] = container.id_arr[j*_id_bytes + i];// j = packetId, 
      result &= txFile(&p,blk_id);
    }
  }else{
    for(size_t j = 0; result && j < container.count; ++j) {// packet sending
      for(size_t i = 0; i < _id_bytes; ++i) blk_id[i] = container.id_arr[j*_id_bytes + i];// j = packetId, 
      result &= tx(&p, container.data_arr[j], container.len_arr[j], blk_id);// gfb, aqui ha d'anar l'arxiu
    }
  }

  if(result) {
    result = close_tx();
  } else {
    //An unrecoverable error occured send cancels to terminate the transaction
    _serial->write(CAN);
    _serial->write(CAN);
    _serial->write(CAN);
  }

  free(buffer);
  return result;
}
void XModem::calc_chksum (byte *data, size_t dataSize, byte *chksum){
  if(_protocol == ProtocolType::XMODEM){
    basic_chksum(data,dataSize,chksum);
  }else{
    crc_16_chksum(data,dataSize,chksum);
  }
}
bool XModem::openFiles(const char * filePath, bool write){
    if(write){
      if(SD.exists(filePath)){
        if(!_onXmodemUpdateHandler(1,1))return false;/** delete file warning */
        SD.remove(filePath);
      }
      workingFile = SD.open(filePath, FILE_WRITE);
    }else{
        workingFile = SD.open(filePath, FILE_READ);
        workingFile.seek(0);
    }
    
    return true;
}
void XModem::closeFiles(bool success){
    workingFile.close();
}
// INTERNAL RECEIVE METHODS
bool XModem::init_rx() {
  byte i = 0;
  do {
    _serial->write(_rx_init_byte);
    if(find_byte_timed(SOH, 10)) return true;
  } while(i++ < retry_limit);
  return false;
}

bool XModem::find_header() {
  byte i = 0;
  do {
    if(i != 0) _serial->write(NAK);
    if(find_byte_timed(SOH, 10)) return true;
  } while(i++ < retry_limit);
  return false;
}
/**
 * Equals bool _xmodem_rx(int *fd, struct xmodem_config *config) <-- linux
*/
bool XModem::rx() {
  bool result = false;
  sizeReceived = 0;
  byte *buffer;
  byte *prev_blk_id;
  byte * expected_id;
  struct packet p;

  //bundle all our memory allocations together
  if(_buffer_packet_reads) {
    //need to store:
    //5 id blocks - prev_blk_id, expected_id, packet struct, buffer id and buffer compl_id
    //2 chksum block - packet struct and buffer chksum
    //2 data block - packet struct and buffer data
    
    buffer = (byte *) malloc(5*_id_bytes + 2*_chksum_bytes + 2*_data_bytes);

    prev_blk_id = buffer + 2*_id_bytes + _chksum_bytes + _data_bytes;
  } else {
    //need to store:
    //3 id blocks - prev_blk_id, expected_id and packet struct
    //1 checksum block - packet struct
    //1 data block - packet struct
    buffer = (byte *) malloc(3*_id_bytes + _chksum_bytes + _data_bytes);
    prev_blk_id = buffer;
  }

  expected_id = prev_blk_id + _id_bytes;
  p.id = expected_id + _id_bytes;
  p.chksum = p.id + _id_bytes;
  p.data = p.chksum + _chksum_bytes;

  for(size_t i = 0; i < _id_bytes; ++i) prev_blk_id[i] = expected_id[i] = 0;

  byte errors = 0;
  while(true) {
    if(read_block(&p, buffer)) {
      //reset errors
      errors = 0;

      //ignore resends of the last received block
      size_t matches = 0;
      for(size_t i = 0; i < _id_bytes; ++i) {
        if(prev_blk_id[i] == p.id[i]) ++matches;
      }

      //if its a duplicate block we still need to send an ACK
      if(matches != _id_bytes) {
        if(_allow_nonsequential) {
          for(size_t i = 0; i < _id_bytes; ++i) expected_id[i] = p.id[i];
        } else {
          increment_id(expected_id, _id_bytes);

          matches = 0;
          for(size_t i = 0; i < _id_bytes; ++i) {
            if(expected_id[i] == p.id[i]) ++matches;
          }

          if(matches != _id_bytes) {
            this->_onXmodemUpdateHandler(3,0);
            break;
            }
        }

        
        if(binary){// process packet, binary mode
          if(!dummy_rx_block_handler(p.id, _id_bytes, p.data, _data_bytes)) break;
        }else{// process packet, text mode
          size_t padding_bytes = 0;
          //count number of padding SUB bytes
          while(p.data[_data_bytes - 1 - padding_bytes] == SUB){
            ++padding_bytes;
          }
          if(!dummy_rx_block_handler(p.id, _id_bytes, p.data, _data_bytes - padding_bytes)) break;
        }
        
        

        for(size_t i = 0; i < _id_bytes; ++i) prev_blk_id[i] = expected_id[i];
      }else{
        this->_onXmodemUpdateHandler(3,1);
      }

      //signal acknowledgment
      byte response = tx_signal(ACK);
      if(response == CAN) break;
      if(response == EOT) {
        response = tx_signal(NAK);
        if(response == CAN) break; // This is not strictly neccessary
        if(response == EOT) {
          _serial->write(ACK);
          result = true;
          closeFiles(true);
          // * to handle end of file receiving here
          break;
        }
      }
      // Unexpected response and resync attempt failed so fail out
      if(response != SOH && !find_header()) break;
    } else {
      this->_onXmodemUpdateHandler(3,2);
      if(++errors > retry_limit) {
        this->_onXmodemUpdateHandler(3,3);
        break;// packet error
      }
      byte response = tx_signal(NAK);
      if(response == CAN) {
        this->_onXmodemUpdateHandler(3,4);
        break;
      }
      if(response != SOH && !find_header()) {
        this->_onXmodemUpdateHandler(3,5);
        break;
        }
    }
  }

  free(buffer);
  closeFiles(false);
  return result;
}

bool XModem::read_block(struct packet *p, byte *buffer) {
  if(_buffer_packet_reads) {
    return read_block_buffered(p, buffer);
  } else {
    return read_block_unbuffered(p);
  }
}

bool XModem::read_block_buffered(struct packet *p, byte *buffer) {
  size_t b_pos = 2*_id_bytes + _data_bytes + _chksum_bytes;
  if(!fill_buffer(buffer, b_pos)) return false;

  b_pos = 0;
  for(size_t i = 0; i < _id_bytes; ++i) {
    p->id[i] = buffer[b_pos++];
    //Because of C integer promotion rules the ~ operator changes
    //the variable type of an unsigned char (byte) to a char so we need to
    //cast it back, lol
    if(p->id[i] != (byte) ~buffer[b_pos++]) return false;
  }

  for(size_t i = 0; i < _data_bytes; ++i) {
    p->data[i] = buffer[b_pos++];
  }

  calc_chksum(p->data, _data_bytes, p->chksum); // rx 2 call
  for(size_t i = 0; i < _chksum_bytes; ++i) {// compara el buffer amb p->chksum[i]. rx
    if(p->chksum[i] != buffer[b_pos++]) return false;
  }

  return true;
}

bool XModem::read_block_unbuffered(struct packet *p) {
  byte tmp;
  for(size_t i = 0; i < _id_bytes; ++i) {
    if(!_serial->readBytes(p->id + i, 1)) return false;
    if(!_serial->readBytes(&tmp, 1)) return false;

    //Because of C integer promotion rules the ~ operator changes
    //the variable type of an unsigned char (byte) to a char so we need to
    //cast it back
    if(p->id[i] != (byte) ~tmp) return false;
  }

  if(!fill_buffer(p->data, _data_bytes)) return false;

  calc_chksum(p->data, _data_bytes, p->chksum); // rx calc unbuffered
  for(size_t i = 0; i < _chksum_bytes; ++i) {// compara els bytes que va rebent amb p->chksum[i]
    if(!_serial->readBytes(&tmp, 1)) return false;
    if(p->chksum[i] != tmp) return false;
  }

  return true;
}

bool XModem::fill_buffer(byte *buffer, size_t bytes) {
  size_t count = 0;
  while(count < bytes) {
    size_t r = _serial->readBytes(buffer + count, bytes - count);

    //the baud rate / sending device may be much slower than ourselves so we
    //only signal an error condition if no data has been received at all within
    //the serial timeout period
    if(r == 0) return false;

    count += r;
  }
  return true;
}

// INTERNAL SEND METHODS
bool XModem::init_tx() {
  byte i = 0;
  do {
    if(find_byte_timed(_rx_init_byte, 60)) return true;
  } while(i++ < retry_limit);
  return false;
}

bool XModem::tx(struct packet *p, byte *data, size_t data_len, byte *blk_id) {
  byte *data_ptr = data;
  byte *data_end = data_ptr + data_len;


  //flush incoming data before starting
  while(_serial->available()) _serial->read();

  if(data == NULL) {
    //need to use block_lookup to fill in the packet data
    build_packet(p, blk_id, NULL, _data_bytes);
    return send_packet(p);
  }

  while(data_ptr + _data_bytes < data_end) {
    build_packet(p, blk_id, data_ptr, _data_bytes);
    increment_id(blk_id, _id_bytes);
    if(!send_packet(p)) return false;
    data_ptr += _data_bytes;
  }

  if(data_ptr != data_end) {
    memset(p->data, SUB, _data_bytes);

    build_packet(p, blk_id, data_ptr, data_end - data_ptr);
    if(!send_packet(p)) return false;
  }

  return true;
}
bool XModem::txFile(struct packet *p,byte *blk_id) {
  
  //flush incoming data before starting
  while(_serial->available()) _serial->read();
  
  while (workingFile.available() > 0) {
    size_t bytesRead = workingFile.read(p->data, _data_bytes);
    if(bytesRead < _data_bytes){// send loop
      for (int i = _data_bytes - (_data_bytes - bytesRead); i < _data_bytes; i++) {
        p->data[i] = 0x1A;
      }
    }
    build_packet(p, blk_id, p->data, _data_bytes);
    increment_id(blk_id, _id_bytes);
    if(!send_packet(p)) return false;
    if(!this->_onXmodemUpdateHandler(0,(uint8_t)*blk_id)) return false;/** reports a transmitted ok packet */
  }

  return true;
}
/**
 * tx call
*/
void XModem::build_packet(struct packet *p, byte *id, byte *data, size_t data_len) {
  memcpy(p->id, id, _id_bytes);
  if(data == NULL) dummy_block_lookup(id, _id_bytes, p->data, data_len);
  else memcpy(p->data, data, data_len);
  calc_chksum(p->data, _data_bytes, p->chksum); // tx calc
}

bool XModem::send_packet(struct packet *p) {
  byte tries = 0;
  do {
    _serial->write(SOH);

    for(size_t i = 0; i < _id_bytes; ++i) {
      _serial->write(p->id[i]);
      _serial->write(~p->id[i]);
    }

    _serial->write(p->data, _data_bytes);
    _serial->write(p->chksum, _chksum_bytes);

    byte response = rx_signal();
    if(response == ACK) return true; // add this->_onXmodemUpdateHandler(...
    if(response == NAK) {
      this->_onXmodemUpdateHandler(3,6);
      continue;
      }
    if(response == CAN) {
      this->_onXmodemUpdateHandler(3,8);
      response = rx_signal();
      if(response == CAN) {
        this->_onXmodemUpdateHandler(3,9);
        break;
      }
    }
  } while(tries++ < retry_limit);

  return false;
}

bool XModem::close_tx() {
  byte error_responses = 0;
  while(error_responses < retry_limit) {
    byte response = tx_signal(EOT);
    if(response == ACK) return true;
    if(response == NAK) continue;
    if(response == CAN) {
      if(rx_signal() == CAN) break;
    } else ++error_responses;
  }
  return false;
}

// INTERNAL SHARED METHODS
void XModem::increment_id(byte *id, size_t length) {
  size_t index = length-1;
  do {
    id[index]++;
    if(id[index]) return; //if the current byte is non-zero then there is no overflow and we are done
  } while(index--); //when our index is zero before decrementing then we have incremented all the bytes
}

byte XModem::tx_signal(byte signal) {
  if(signal == NAK) {
    //flush to make sure the line is clear
    while(_serial->available()) _serial->read();
  }
  byte i = 0;
  byte val;
  do {
    byte read_attempt = 0;
    _serial->write(signal);
    while(_serial->readBytes(&val, 1) == 0 && read_attempt++ < retry_limit) delay(_signal_retry_delay_ms);

    switch(val) {
      case SOH:
      case EOT:
      case CAN:
      case ACK:
      case NAK:
        return val;
    }
  } while(++i < retry_limit);
  return 255;
}

byte XModem::rx_signal() {
  byte i = 0;
  byte val;
  while(_serial->readBytes(&val, 1) == 0 && ++i < retry_limit) delay(_signal_retry_delay_ms);

  switch(val) {
    case ACK:
    case NAK:
    case CAN:
      return val;
  }
  return 255;
}

bool XModem::find_byte_timed(byte b, byte timeout_secs) {
  unsigned long end = millis() + ((unsigned long) timeout_secs * 1000UL);
  do {
    if(_serial->find(b)) return true;
  } while(millis() < end);
  return false;
}

/**
 * https://github.com/arduino-libraries/SD/blob/85dbcca432d1b658d0d848f5f7ba0a9e811afc04/examples/NonBlockingWrite/NonBlockingWrite.ino
*/
bool XModem::dummy_rx_block_handler(byte *blk_id, size_t idSize, byte *data, size_t dataSize) {

  if(binary){ // receive consequent loop
    sizeReceived += dataSize;
    if(sizeReceived > sizeKnown){
      if((sizeReceived-sizeKnown) > dataSize){// if sizeKnown is wrong, not correct
        return false;
      }
      workingFile.write(data,(sizeKnown -(sizeReceived -dataSize)));// has d'escriure el tros que ell diu
    }else{
      workingFile.write(data,dataSize);// has d'escriure el tros que ell diu
    }
  }else{
    workingFile.write(data,dataSize);// has d'escriure el tros que ell diu
  }
  workingFile.flush();
  if(!this->_onXmodemUpdateHandler(0,(uint8_t)*blk_id)) return false;/** reports a received ok packet */
  return true;
}

void XModem::dummy_block_lookup(void *blk_id, size_t idSize, byte *send_data, size_t dataSize) {
  memset(send_data, 0x3A, dataSize);
}

void XModem::basic_chksum(byte *data, size_t dataSize, byte *chksum) {
  byte sum = 0;
  for(size_t i = 0; i < dataSize; ++i) sum += data[i];
  *chksum = sum;
}

 void XModem::crc_16_chksum(byte *data, size_t dataSize, byte *chksum) {
  // s'envia p->chksum
  //XModem CRC prime number is 69665 -> 2^16 + 2^12 + 2^5 + 2^0 -> 10001000000100001 -> 0x11021
  //normal notation of this bit pattern omits the leading bit and represents it as 0x1021
  //in code we can omit the 2^16 term due to shifting before XORing when the MSB is a 1
  unsigned short crc = 0xFFFF; // Initial value
  unsigned polynomial = 0xA001; // CRC-16 polynomial
  for(size_t i = 0; i < dataSize; ++i) {
      crc ^= data[i];

        for (int i = 0; i < 8; i++)
        {
            if ((crc & 0x0001) == 0x0001)
            {
                crc >>= 1;
                crc ^= polynomial;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
  

/*
  //------------------------------------
  //const unsigned short crc_prime = 0x1021;
  //unsigned short *crc = (unsigned short *) chksum;
  //*crc = 0;// unhandled_user_irq_num_in_r0 --> https://forums.raspberrypi.com/viewtopic.php?t=320355

  //We can ignore crc calulations that cross byte boundaries by just assuming
  //that the following byte is 0 and then fixup our simplification at the end
  //by XORing in the true value of the next byte into the most sygnificant byte
  //of the CRC
  for(size_t i = 0; i < dataSize; ++i) {
    *crc ^= (((unsigned short) data[i]) << 8);
    for(byte j = 0; j < 8; ++j) {
      if(*crc & 0x8000) *crc = (*crc << 1) ^ crc_prime;
      else *crc <<= 1;
    }
  }
  // aqui crc ha quedat establert
  */
}
// ho has de cridar  atrave´s del extern
XModem myXModem;