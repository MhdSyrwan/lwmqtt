#include <stdbool.h>
#include <string.h>

#include "helpers.h"
#include "packet.h"

int lwmqtt_encode_remaining_length(unsigned char *buf, int rem_len) {
  // init len counter
  int len = 0;

  // encode variadic number
  do {
    // calculate current digit
    unsigned char d = (unsigned char)(rem_len % 128);

    // change remaining length
    rem_len /= 128;

    // if there are more digits to encode, set the top bit of this digit
    if (rem_len > 0) {
      d |= 0x80;
    }

    // set digit
    buf[len++] = d;
  } while (rem_len > 0);

  return len;
}

int lwmqtt_total_header_length(int rem_len) {
  if (rem_len < 128) {
    return 1 + 1;
  } else if (rem_len < 16384) {
    return 1 + 2;
  } else if (rem_len < 2097151) {
    return 1 + 3;
  } else {
    return 1 + 4;
  }
}

// TODO: Increment pointer directly?
int lwmqtt_decode_remaining_length(unsigned char *buf, int *rem_len) {
  unsigned char c;
  int multiplier = 1;
  int len = 0;

  *rem_len = 0;
  do {
    len++;

    if (len > 4) {
      return LWMQTT_REMAINING_LENGTH_OVERFLOW;
    }

    c = buf[len - 1];

    *rem_len += (c & 127) * multiplier;
    multiplier *= 128;
  } while ((c & 128) != 0);

  return len;
}

typedef union {
  unsigned char byte;

  struct {
    unsigned int _ : 1;
    unsigned int clean_session : 1;
    unsigned int will : 1;
    unsigned int will_qos : 2;
    unsigned int will_retain : 1;
    unsigned int password : 1;
    unsigned int username : 1;
  } bits;
} lwmqtt_connect_flags_t;

typedef union {
  unsigned char byte;

  struct {
    unsigned int _ : 7;
    unsigned int session_present : 1;
  } bits;
} lwmqtt_connack_flags_t;

lwmqtt_err_t lwmqtt_encode_connect(unsigned char *buf, int buf_len, int *len, lwmqtt_options_t *options,
                                   lwmqtt_will_t *will) {
  // prepare pointer
  unsigned char *ptr = buf;

  /* calculate remaining length */

  // fixed header is 10
  int rem_len = 10;

  // add client id
  rem_len += lwmqtt_strlen(options->client_id) + 2;

  // add will if present
  if (will != NULL) {
    rem_len += lwmqtt_strlen(will->topic) + 2 + will->payload_len + 2;
  }

  // add username if present
  if (options->username.c_string || options->username.lp_string.data) {
    rem_len += lwmqtt_strlen(options->username) + 2;

    // add password if present
    if (options->password.c_string || options->password.lp_string.data) {
      rem_len += lwmqtt_strlen(options->password) + 2;
    }
  }

  // check buffer capacity
  if (lwmqtt_total_header_length(rem_len) + rem_len > buf_len) {
    return LWMQTT_BUFFER_TOO_SHORT_ERROR;
  }

  /* encode packet */

  // write header
  lwmqtt_header_t header = {0};
  header.bits.type = LWMQTT_CONNECT_PACKET;
  lwmqtt_write_char(&ptr, header.byte);

  // write remaining length
  ptr += lwmqtt_encode_remaining_length(ptr, rem_len);

  // write version
  lwmqtt_write_c_string(&ptr, "MQTT");
  lwmqtt_write_char(&ptr, 4);

  // prepare flags
  lwmqtt_connect_flags_t flags = {0};
  flags.bits.clean_session = options->clean_session ? 1 : 0;

  // set will flags if present
  if (will != NULL) {
    flags.bits.will = 1;
    flags.bits.will_qos = (unsigned int)will->qos;
    flags.bits.will_retain = will->retained ? 1 : 0;
  }

  // set username flag if present
  if (options->username.c_string || options->username.lp_string.data) {
    flags.bits.username = 1;

    // set password flag if present
    if (options->password.c_string || options->password.lp_string.data) {
      flags.bits.password = 1;
    }
  }

  // write flags
  lwmqtt_write_char(&ptr, flags.byte);

  // write keep alive
  lwmqtt_write_int(&ptr, options->keep_alive);

  // write client id
  lwmqtt_write_string(&ptr, options->client_id);

  // write will topic and payload if present
  if (will != NULL) {
    lwmqtt_write_string(&ptr, will->topic);
    lwmqtt_write_int(&ptr, will->payload_len);
    memcpy(ptr, will->payload, will->payload_len);
    ptr += will->payload_len;
  }

  // write username if present
  if (flags.bits.username) {
    lwmqtt_write_string(&ptr, options->username);

    // write password if present
    if (flags.bits.password) {
      lwmqtt_write_string(&ptr, options->password);
    }
  }

  // set written length
  *len = (int)(ptr - buf);

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_decode_connack(bool *session_present, lwmqtt_connack_t *connack_rc, unsigned char *buf,
                                   int buf_len) {
  // prepare pointer
  unsigned char *ptr = buf;

  // read header
  lwmqtt_header_t header;
  header.byte = lwmqtt_read_char(&ptr);
  if (header.bits.type != LWMQTT_CONNACK_PACKET) {
    return LWMQTT_FAILURE;
  }

  // read remaining length
  int len;
  if (lwmqtt_decode_remaining_length(ptr, &len) == LWMQTT_REMAINING_LENGTH_OVERFLOW) {
    return LWMQTT_REMAINING_LENGTH_OVERFLOW;
  }

  // check lengths
  if (len != 2 || buf_len < len + 2) {
    return LWMQTT_LENGTH_MISMATCH;
  }

  // advance pointer
  ptr++;

  // read flags
  lwmqtt_connack_flags_t flags;
  flags.byte = lwmqtt_read_char(&ptr);
  *session_present = flags.bits.session_present == 1;
  *connack_rc = (lwmqtt_connack_t)lwmqtt_read_char(&ptr);

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_encode_zero(unsigned char *buf, int buf_len, int *len, lwmqtt_packet_t packet) {
  // prepare pointer
  unsigned char *ptr = buf;

  // check buffer length
  if (buf_len < 2) {
    return LWMQTT_BUFFER_TOO_SHORT_ERROR;
  }

  // write header
  lwmqtt_header_t header = {0};
  header.bits.type = packet;
  lwmqtt_write_char(&ptr, header.byte);

  // write remaining length
  ptr += lwmqtt_encode_remaining_length(ptr, 0);

  // set length
  *len = (int)(ptr - buf);

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_decode_ack(lwmqtt_packet_t *packet_type, bool *dup, unsigned short *packet_id, unsigned char *buf,
                               int buf_len) {
  // prepare pointer
  unsigned char *cur_ptr = buf;

  // read header
  lwmqtt_header_t header = {0};
  header.byte = lwmqtt_read_char(&cur_ptr);
  *dup = header.bits.dup == 1;
  *packet_type = (lwmqtt_packet_t)header.bits.type;

  // read remaining length
  int rem_len;
  if (lwmqtt_decode_remaining_length(cur_ptr, &rem_len) == LWMQTT_REMAINING_LENGTH_OVERFLOW) {
    return LWMQTT_REMAINING_LENGTH_OVERFLOW;
  }

  // check lengths
  if (rem_len != 2 || buf_len < rem_len + 2) {
    return LWMQTT_LENGTH_MISMATCH;
  }

  // advance pointer
  cur_ptr++;

  // read packet id
  *packet_id = (unsigned short)lwmqtt_read_int(&cur_ptr);

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_encode_ack(unsigned char *buf, int buf_len, int *len, lwmqtt_packet_t packet, bool dup,
                               unsigned short packet_id) {
  // prepare pointer
  unsigned char *ptr = buf;

  // check buffer size
  if (buf_len < 4) {
    return LWMQTT_BUFFER_TOO_SHORT_ERROR;
  }

  // write header
  lwmqtt_header_t header = {0};
  header.bits.type = packet;
  header.bits.dup = dup ? 1 : 0;
  header.bits.qos = (packet == LWMQTT_PUBREL_PACKET) ? 1 : 0;
  lwmqtt_write_char(&ptr, header.byte);

  // write remaining length
  ptr += lwmqtt_encode_remaining_length(ptr, 2);

  // write packet id
  lwmqtt_write_int(&ptr, packet_id);

  // set written length
  *len = (int)(ptr - buf);

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_decode_publish(bool *dup, lwmqtt_qos_t *qos, bool *retained, unsigned short *packet_id,
                                   lwmqtt_string_t *topic, unsigned char **payload, int *payload_len,
                                   unsigned char *buf, int buf_len) {
  // prepare pointer
  unsigned char *ptr = buf;

  // read header
  lwmqtt_header_t header;
  header.byte = lwmqtt_read_char(&ptr);
  if (header.bits.type != LWMQTT_PUBLISH_PACKET) {
    return LWMQTT_FAILURE;
  }

  // set dup
  *dup = header.bits.dup == 1;

  // set qos
  *qos = (lwmqtt_qos_t)header.bits.qos;

  // set retained
  *retained = header.bits.retain == 1;

  // read remaining length
  int rem_len = 0;
  int rc = lwmqtt_decode_remaining_length(ptr, &rem_len);
  ptr += rc;

  // calculate end pointer
  unsigned char *end_ptr = ptr + rem_len;

  // do we have enough data to read the topic?
  if (!lwmqtt_read_lp_string(topic, &ptr, end_ptr) || end_ptr - ptr < 0) {
    return LWMQTT_FAILURE;
  }

  // read packet id if qos is at least 1
  if (*qos > 0) {
    *packet_id = (unsigned short)lwmqtt_read_int(&ptr);
  }

  // set payload
  *payload_len = (int)(end_ptr - ptr);
  *payload = ptr;

  return LWMQTT_SUCCESS;
}

lwmqtt_err_t lwmqtt_encode_publish(unsigned char *buf, int buf_len, int *len, bool dup, lwmqtt_qos_t qos, bool retained,
                                   unsigned short packet_id, lwmqtt_string_t topic, unsigned char *payload,
                                   int payload_len) {
  // prepare pointer
  unsigned char *ptr = buf;

  // prepare remaining length
  int rem_len = 2 + lwmqtt_strlen(topic) + payload_len;

  // add packet id if qos is at least 1
  if (qos > 0) {
    rem_len += 2;
  }

  // check buffer size
  if (lwmqtt_total_header_length(rem_len) + rem_len > buf_len) {
    return LWMQTT_BUFFER_TOO_SHORT_ERROR;
  }

  // write header
  lwmqtt_header_t header = {0};
  header.bits.type = LWMQTT_PUBLISH_PACKET;
  header.bits.dup = dup ? 1 : 0;
  header.bits.qos = (unsigned int)qos;
  header.bits.retain = retained ? 1 : 0;
  lwmqtt_write_char(&ptr, header.byte);

  // write remaining length
  ptr += lwmqtt_encode_remaining_length(ptr, rem_len);

  // write topic
  lwmqtt_write_string(&ptr, topic);

  // write packet id if qos is at least 1
  if (qos > 0) {
    lwmqtt_write_int(&ptr, packet_id);
  }

  // write payload
  memcpy(ptr, payload, payload_len);
  ptr += payload_len;

  // set length
  *len = (int)(ptr - buf);

  return LWMQTT_SUCCESS;
}
