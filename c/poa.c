// # PoA
//
// A lock script used for proof of authority governance on CKB.

// Due to the way CKB works, shared state in dapps is a common problem requiring
// special care. One naive solution, is to introduce a certain kind of
// aggregator, that would pack multiple invididual actions on the sahred state
// into a single CKB transaction. But one issue with aggregator is
// centralization: with one aggregator, the risk of censoring is quite high.
// This script provides a simple attempt at the problem: we will just use
// multiple aggregators! Each aggregator can only issue new transaction(s) when
// their round is reached. Notice that this is by no means the solution to the
// problem we are facing. Many better attempts are being built, the lock script
// here, simply is built to show one of many possibilities on CKB, and help
// inspire new ideas.

// Terminologies:
// * Subblock: a CKB transaction generated by the aggregator, which can contain
// multiple individual actions. It's like a layer 2 block except all validations
// here happens on layer 1 CKB.
// * Subtime: timestamp, or block number for a subblock.
// * Interval: duration in which only one designated aggregator can issue new
// subblocks, measured in subtime.
// * Round: a single interval duration. One aggregator could issue more than one
// subblock in its round.

// As always, we will need those headers to interact with CKB.
#include "blake2b.h"
#include "blockchain.h"
#include "ckb_syscalls.h"

#define SCRIPT_BUFFER_SIZE 128
#define POA_BUFFER_SIZE 16384
#define SIGNATURE_WITNESS_BUFFER_SIZE 32768
#define ONE_BATCH_SIZE 32768
#define CODE_SIZE (256 * 1024)
#define PREFILLED_DATA_SIZE (1024 * 1024)
#define IDENTITY_SIZE 32

#define ERROR_TRANSACTION -1
#define ERROR_ENCODING -2
#define ERROR_DYNAMIC_LOADING -3

#ifdef ENABLE_DEBUG_MODE
#define DEBUG(s) ckb_debug(s)
#else
#define DEBUG(s)
#endif /* ENABLE_DEBUG_MODE */

typedef struct {
  const uint8_t *_source_data;
  size_t _source_length;

  int interval_uses_seconds;
  uint8_t identity_size;
  uint8_t aggregator_number;
  uint8_t aggregator_change_threshold;
  uint32_t subblock_intervals;
  uint32_t subblocks_per_interval;
  const uint8_t *identities;
} PoASetup;

int parse_poa_setup(const uint8_t *source_data, size_t source_length,
                    PoASetup *output) {
  if (source_length < 12) {
    DEBUG("PoA data have invalid length!");
    return ERROR_ENCODING;
  }
  output->_source_data = source_data;
  output->_source_length = source_length;

  output->interval_uses_seconds = (source_data[0] & 1) == 1;
  output->identity_size = source_data[1];
  output->aggregator_number = source_data[2];
  output->aggregator_change_threshold = source_data[3];
  output->subblock_intervals = *((uint32_t *)(&source_data[4]));
  output->subblocks_per_interval = *((uint32_t *)(&source_data[8]));
  output->identities = &source_data[12];

  if (output->identity_size > IDENTITY_SIZE) {
    DEBUG("Invalid identity size!");
    return ERROR_ENCODING;
  }
  if (output->aggregator_change_threshold > output->aggregator_number) {
    DEBUG("Invalid aggregator change threshold!");
    return ERROR_ENCODING;
  }
  if (source_length !=
      12 + (size_t)output->identity_size * (size_t)output->aggregator_number) {
    DEBUG("PoA data have invalid length!");
    return ERROR_ENCODING;
  }
  return CKB_SUCCESS;
}

int validate_consensus_signing(const uint8_t *identity_buffer,
                               size_t identity_size, uint8_t identity_count,
                               uint8_t aggregator_change_threshold) {
  uint64_t mask[4];
  mask[0] = mask[1] = mask[2] = mask[3] = 0;
  uint8_t found = 0;
  size_t current = 0;
  while (current < SIZE_MAX) {
    uint64_t len = 32;
    uint8_t hash[32];

    int ret = ckb_load_cell_by_field(hash, &len, 0, current, CKB_SOURCE_INPUT,
                                     CKB_CELL_FIELD_LOCK_HASH);
    if (ret == CKB_INDEX_OUT_OF_BOUND) {
      break;
    }
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    uint8_t found_identity = 0;
    for (; found_identity < identity_count; found_identity++) {
      int found =
          ((mask[found_identity / 64] >> (found_identity % 64)) & 1) != 0;
      if ((!found) &&
          memcmp(hash, &identity_buffer[found_identity * identity_size],
                 identity_size) == 0) {
        break;
      }
    }
    if (found_identity < identity_count) {
      // New match found
      found++;
      if (found == aggregator_change_threshold) {
        return CKB_SUCCESS;
      }
      mask[found_identity / 64] |= 1 << (found_identity % 64);
    }
    current++;
  }
  DEBUG("Not enough matching identities found!");
  return ERROR_ENCODING;
}

int validate_single_signing(const uint8_t *identity, size_t identity_size) {
  size_t current = 0;
  while (current < SIZE_MAX) {
    uint64_t len = 32;
    uint8_t hash[32];

    int ret = ckb_load_cell_by_field(hash, &len, 0, current, CKB_SOURCE_INPUT,
                                     CKB_CELL_FIELD_LOCK_HASH);
    if (ret == CKB_INDEX_OUT_OF_BOUND) {
      break;
    }
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    if (memcmp(hash, identity, identity_size) == 0) {
      return CKB_SUCCESS;
    }
    current++;
  }
  DEBUG("No matching identity found!");
  return ERROR_ENCODING;
}

static const uint8_t type_id_script_prefix[53] = {
    0x55, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00,
    0x00, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x59, 0x50,
    0x45, 0x5f, 0x49, 0x44, 0x01, 0x20, 0x00, 0x00, 0x00};

int look_for_poa_cell(const uint8_t *type_id, size_t source, size_t *index) {
  size_t current = 0;
  size_t found_index = SIZE_MAX;
  int running = 1;
  while ((running == 1) && (current < SIZE_MAX)) {
    uint64_t len = 85;
    uint8_t script[85];

    int ret = ckb_load_cell_by_field(script, &len, 0, current, source,
                                     CKB_CELL_FIELD_TYPE);
    switch (ret) {
      case CKB_ITEM_MISSING:
        break;
      case CKB_SUCCESS:
        if (len == 85 && memcmp(type_id_script_prefix, script, 53) == 0 &&
            memcmp(type_id, &script[53], 32) == 0) {
          // Found a match;
          if (found_index != SIZE_MAX) {
            // More than one PoA cell exists
            DEBUG("Duplicate PoA cell!");
            return ERROR_ENCODING;
          }
          found_index = current;
        }
        break;
      default:
        running = 0;
        break;
    }
    current++;
  }
  if (found_index == SIZE_MAX) {
    return CKB_INDEX_OUT_OF_BOUND;
  }
  *index = found_index;
  return CKB_SUCCESS;
}

int main() {
  // One CKB transaction can only have one cell using current lock.
  uint64_t len = 0;
  int ret = ckb_load_cell(NULL, &len, 0, 1, CKB_SOURCE_GROUP_INPUT);
  if (ret != CKB_INDEX_OUT_OF_BOUND) {
    DEBUG("Transaction has more than one input cell using current lock!");
    return ERROR_TRANSACTION;
  }
  len = 0;
  ret = ckb_load_cell(NULL, &len, 0, 1, CKB_SOURCE_GROUP_OUTPUT);
  if (ret != CKB_INDEX_OUT_OF_BOUND) {
    DEBUG("Transaction has more than one output cell using current lock!");
    return ERROR_TRANSACTION;
  }

  // Load current script so as to extract PoA cell information
  unsigned char script[SCRIPT_BUFFER_SIZE];
  len = SCRIPT_BUFFER_SIZE;
  ret = ckb_checked_load_script(script, &len, 0);
  if (ret != CKB_SUCCESS) {
    return ret;
  }

  mol_seg_t script_seg;
  script_seg.ptr = (uint8_t *)script;
  script_seg.size = len;
  if (MolReader_Script_verify(&script_seg, false) != MOL_OK) {
    DEBUG("molecule verification failure!");
    return ERROR_ENCODING;
  }
  mol_seg_t args_seg = MolReader_Script_get_args(&script_seg);
  mol_seg_t args_bytes_seg = MolReader_Bytes_raw_bytes(&args_seg);

  if (args_bytes_seg.size != 64) {
    DEBUG("Script args must be 64 bytes long!");
    return ERROR_ENCODING;
  }

  size_t dep_poa_setup_cell_index = SIZE_MAX;
  ret = look_for_poa_cell(args_bytes_seg.ptr, CKB_SOURCE_CELL_DEP,
                          &dep_poa_setup_cell_index);
  if (ret != CKB_INDEX_OUT_OF_BOUND && ret != CKB_SUCCESS) {
    return ret;
  }
  if (ret == CKB_SUCCESS) {
    // Normal new blocks
    uint8_t dep_poa_setup_buffer[POA_BUFFER_SIZE];
    uint64_t len = POA_BUFFER_SIZE;
    ret = ckb_load_cell_data(dep_poa_setup_buffer, &len, 0,
                             dep_poa_setup_cell_index, CKB_SOURCE_CELL_DEP);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    if (len > POA_BUFFER_SIZE) {
      DEBUG("Dep PoA cell is too large!");
      return ERROR_ENCODING;
    }
    PoASetup poa_setup;
    ret = parse_poa_setup(dep_poa_setup_buffer, len, &poa_setup);
    if (ret != CKB_SUCCESS) {
      return ret;
    }

    size_t input_poa_data_cell_index = SIZE_MAX;
    ret = look_for_poa_cell(&args_bytes_seg.ptr[32], CKB_SOURCE_INPUT,
                            &input_poa_data_cell_index);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    uint8_t input_poa_data_buffer[22];
    len = 22;
    ret = ckb_load_cell_data(input_poa_data_buffer, &len, 0,
                             input_poa_data_cell_index, CKB_SOURCE_INPUT);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    if (len != 22) {
      DEBUG("Invalid input poa data cell!");
      return ERROR_ENCODING;
    }
    const uint8_t *last_subblock_info = input_poa_data_buffer;

    size_t output_poa_data_cell_index = SIZE_MAX;
    ret = look_for_poa_cell(&args_bytes_seg.ptr[32], CKB_SOURCE_OUTPUT,
                            &output_poa_data_cell_index);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    uint8_t output_poa_data_buffer[22];
    len = 22;
    ret = ckb_load_cell_data(output_poa_data_buffer, &len, 0,
                             output_poa_data_cell_index, CKB_SOURCE_OUTPUT);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    if (len != 22) {
      DEBUG("Invalid output poa data cell!");
      return ERROR_ENCODING;
    }
    const uint8_t *current_subblock_info = output_poa_data_buffer;

    // Check that current aggregator is indeed due to issuing new block.
    uint64_t last_round_initial_subtime = *((uint64_t *)last_subblock_info);
    uint64_t last_subblock_subtime = *((uint64_t *)(&last_subblock_info[8]));
    uint32_t last_block_index = *((uint32_t *)(&last_subblock_info[16]));
    uint16_t last_aggregator_index = *((uint16_t *)(&last_subblock_info[20]));

    uint64_t current_round_initial_subtime =
        *((uint64_t *)current_subblock_info);
    uint64_t current_subblock_subtime =
        *((uint64_t *)(&current_subblock_info[8]));
    uint32_t current_subblock_index =
        *((uint32_t *)(&current_subblock_info[16]));
    uint16_t current_aggregator_index =
        *((uint16_t *)(&current_subblock_info[20]));
    if (current_aggregator_index >= poa_setup.aggregator_number) {
      DEBUG("Invalid aggregator index!");
      return ERROR_ENCODING;
    }

    // Since is used to ensure aggregators wait till the correct time.
    uint64_t since = 0;
    len = 8;
    ret =
        ckb_load_input_by_field(((uint8_t *)&since), &len, 0, 0,
                                CKB_SOURCE_GROUP_INPUT, CKB_INPUT_FIELD_SINCE);
    if (ret != CKB_SUCCESS) {
      return ret;
    }
    if (len != 8) {
      DEBUG("Invalid loading since!");
      return ERROR_ENCODING;
    }
    if (poa_setup.interval_uses_seconds) {
      if (since >> 56 != 0x40) {
        DEBUG("PoA requires absolute timestamp since!");
        return ERROR_ENCODING;
      }
    } else {
      if (since >> 56 != 0) {
        DEBUG("PoA requires absolute block number since!");
        return ERROR_ENCODING;
      }
    }
    since &= 0x00FFFFFFFFFFFFFF;
    if (current_subblock_subtime != since) {
      DEBUG("Invalid current time!");
      return ERROR_ENCODING;
    }

    // There are 2 supporting modes:
    // 1. An aggregator can issue as much new blocks as it wants as long as
    // subblock_intervals and subblocks_per_interval requirement is met.
    // 2. When the subblock_intervals duration has passed, the next aggregator
    // should now be able to issue more blocks.
    if (since < last_round_initial_subtime + poa_setup.subblock_intervals) {
      // Current aggregator is issuing blocks
      if (current_round_initial_subtime != last_round_initial_subtime) {
        DEBUG("Invalid current round first timestamp!");
        return ERROR_ENCODING;
      }
      // Timestamp must be non-decreasing
      if (current_subblock_subtime < last_subblock_subtime) {
        DEBUG("Invalid current timestamp!");
        return ERROR_ENCODING;
      }
      if (current_aggregator_index != last_aggregator_index) {
        DEBUG("Invalid aggregator!");
        return ERROR_ENCODING;
      }
      if ((current_subblock_index != last_block_index + 1) ||
          (current_subblock_index >= poa_setup.subblocks_per_interval)) {
        DEBUG("Invalid block index");
        return ERROR_ENCODING;
      }
    } else {
      if (current_round_initial_subtime != current_subblock_subtime) {
        DEBUG("Invalid current round first timestamp!");
        return ERROR_ENCODING;
      }
      if (current_subblock_index != 0) {
        DEBUG("Invalid block index");
        return ERROR_ENCODING;
      }
      // Next aggregator in place
      uint64_t steps = (((uint64_t)current_aggregator_index +
                         (uint64_t)poa_setup.aggregator_number -
                         (uint64_t)last_aggregator_index) %
                        (uint64_t)poa_setup.aggregator_number);
      if (steps == 0) {
        steps = (uint64_t)poa_setup.aggregator_number;
      }
      uint64_t duration = steps * ((uint64_t)poa_setup.subblock_intervals);
      if (since < duration + last_round_initial_subtime) {
        DEBUG("Invalid time!");
        return ERROR_ENCODING;
      }
    }

    return validate_single_signing(
        &poa_setup.identities[(size_t)current_aggregator_index *
                              (size_t)poa_setup.identity_size],
        poa_setup.identity_size);
  }
  // PoA consensus mode
  size_t input_poa_setup_cell_index = SIZE_MAX;
  ret = look_for_poa_cell(args_bytes_seg.ptr, CKB_SOURCE_INPUT,
                          &input_poa_setup_cell_index);
  if (ret != CKB_SUCCESS) {
    return ret;
  }
  uint8_t input_poa_setup_buffer[POA_BUFFER_SIZE];
  uint64_t input_poa_setup_len = POA_BUFFER_SIZE;
  ret = ckb_load_cell_data(input_poa_setup_buffer, &input_poa_setup_len, 0,
                           input_poa_setup_cell_index, CKB_SOURCE_INPUT);
  if (ret != CKB_SUCCESS) {
    return ret;
  }
  if (input_poa_setup_len > POA_BUFFER_SIZE) {
    DEBUG("Input PoA cell is too large!");
    return ERROR_ENCODING;
  }
  PoASetup poa_setup;
  ret =
      parse_poa_setup(input_poa_setup_buffer, input_poa_setup_len, &poa_setup);
  if (ret != CKB_SUCCESS) {
    return ret;
  }

  size_t output_poa_setup_cell_index = SIZE_MAX;
  ret = look_for_poa_cell(args_bytes_seg.ptr, CKB_SOURCE_OUTPUT,
                          &output_poa_setup_cell_index);
  if (ret != CKB_SUCCESS) {
    return ret;
  }
  uint8_t output_poa_setup_buffer[POA_BUFFER_SIZE];
  uint64_t output_poa_setup_len = POA_BUFFER_SIZE;
  ret = ckb_load_cell_data(output_poa_setup_buffer, &output_poa_setup_len, 0,
                           output_poa_setup_cell_index, CKB_SOURCE_OUTPUT);
  if (ret != CKB_SUCCESS) {
    return ret;
  }
  if (output_poa_setup_len > POA_BUFFER_SIZE) {
    DEBUG("Output PoA cell is too large!");
    return ERROR_ENCODING;
  }
  PoASetup new_poa_setup;
  ret = parse_poa_setup(output_poa_setup_buffer, output_poa_setup_len,
                        &new_poa_setup);
  if (ret != CKB_SUCCESS) {
    return ret;
  }

  return validate_consensus_signing(
      poa_setup.identities, poa_setup.identity_size,
      poa_setup.aggregator_number, poa_setup.aggregator_change_threshold);
}
