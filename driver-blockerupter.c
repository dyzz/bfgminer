/*
 * Copyright 2014 Jason Zong
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "deviceapi.h"
#include "lowlevel.h"
#include "lowl-vcom.h"
#include "miner.h"

#define BET_MAXBOARDS 32
#define BET_MAXASICS  48
#define BET_BAUD 460800
#define BET_TIMEOUT 5

#define BET_CLOCK_MAX 29
#define BET_CLOCK_DEFAULT 23
#define BET_DIFF_DEFAULT 64
#define BET_ROLLING_DEFAULT 5
extern int opt_bet_clk;

#define BET_WORK_FIFO 128
#define BET_NONCE_FIX 4

#define SEND_OK 0
#define SEND_FAIL 1
#define READ_OK 0
#define READ_FAIL 1

// Global Commands
// resets all mega88, recv nothing
#define C_RES	(0 << 5)
// stop jobs on all boards, set nTime rolling to (BoardID+1)*30, recv nothing
#define C_LPO	(1 << 5)
// set clock for all boards, clock = (BoardID+1)*5, recv nothing
#define C_GCK	(2 << 5)
// set difficulty bits for all boards with last 2bits from BoardID, recv nothing
#define C_DIF	(3 << 5)

// Board Specific Commands (CMD|BoardID)
// Send midstate(32 bytes), remaining block header(12 bytes), extranonce2(4 bytes) and job index(1 byte) to board
// Recv 0x58
#define C_JOB	(4 << 5)
// Recv current status of board
#define C_ASK	(5 << 5)
// Recv (max_asics) bytes of chip test result, (max asics) bytes of clocks, 1 byte of diff bits, 1 byte of max nTime rolling, 1 byte of firmware version. Total (max asics)*2+3 bytes
#define C_TRS	(6 << 5)	

// answers on C_ASK|BoardID
// Idle, waiting for new job
#define A_WAL	0x56
// Mining but no nonce yet
#define A_NO	0xa6
// Found nonce, followed with midstate(32 bytes), remaining block header(12 bytes), extranonce2(4 bytes), nonce(4 bytes), job index(1 byte), chip index(1 byte). Total 54 bytes.
#define A_YES	0x5A

// answer on C_JOB|BoardID
#define A_GET   0x58    

#pragma pack(1)

typedef struct asic_info {
	int bad;
	int accepted;
	int nonces;
	int hashes;
	double hwe;
} asic_info;

#pragma pack(1)

typedef struct board_info {
	int bad;
	int job_count;
	int nonces;
	int accepted;
	int hashes;
	double hashrate;
	double hwe;
	struct asic_info asics[BET_MAXASICS];
} board_info;

#pragma pack(1)

typedef struct blockerupter_info {
	struct pool pool;
	uint8_t found;
	int clock;
	int nonces;
	int diff;
	int rolling;
	int accepted;
	int hashes;
	double hashrate;
	double expected;
	double eff;
	uint8_t work_idx;
	struct work works[BET_WORK_FIFO];
	uint8_t boards[BET_MAXBOARDS];
	board_info b_info[BET_MAXBOARDS];
	struct timeval start_time;
	struct timeval last_job;
} blockerupter_info;


#pragma pack(1)

typedef struct blockerupter_response {
	uint8_t midstate[32];
	uint8_t merkle[4];
	uint8_t ntime[4];
	uint8_t diff[4];
	uint8_t exnonc2[4];
	uint8_t nonce[4];
	uint8_t work_idx;
	uint8_t chip;
} blockerupter_response;
#define BET_RESP_SZ (sizeof(blockerupter_response))

static
int blockerupter_open(const char *devpath)
{
    return serial_open(devpath, BET_BAUD, BET_TIMEOUT, true);
}

static
int blockerupter_read(int fd, void *buf, size_t bufLen)
{
    int result;

    result = read(fd, buf, bufLen);

    if(result < 0) {
        applog(LOG_ERR, "ERR");
    }

    if (result != bufLen) {
        applog(LOG_DEBUG, "blockerupter: Read (%d/%zu)", result, bufLen);
    }

    return result;
}


static
int blockerupter_write(int fd, void *buf, size_t bufLen)
{
    int result;

    result = write(fd, buf, bufLen);

    if(result < 0) {
        applog(LOG_ERR, "ERR");
    }

    if (result != bufLen) {
        applog(LOG_DEBUG, "blockerupter: Write (%d/%zu)", result, bufLen);
    }

    return result;
}
