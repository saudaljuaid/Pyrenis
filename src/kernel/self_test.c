/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stdint.h>

#include <seneri/boot.h>
#include <seneri/self_test.h>

struct empty_information {
    struct multiboot2_information_header header;
    struct multiboot2_tag end;
} __attribute__((packed, aligned(8)));

struct bad_end_information {
    struct multiboot2_information_header header;
    struct multiboot2_tag end;
    uint64_t unexpected_tail;
} __attribute__((packed, aligned(8)));

struct unterminated_string_tag {
    struct multiboot2_tag tag;
    char bytes[4];
    uint32_t padding;
} __attribute__((packed));

struct unterminated_string_information {
    struct multiboot2_information_header header;
    struct unterminated_string_tag string;
    struct multiboot2_tag end;
} __attribute__((packed, aligned(8)));

struct test_module_tag {
    struct multiboot2_tag tag;
    uint32_t module_start;
    uint32_t module_end;
    char name[1];
    uint8_t padding[7];
} __attribute__((packed));

struct module_information {
    struct multiboot2_information_header header;
    struct test_module_tag module;
    struct multiboot2_tag end;
} __attribute__((packed, aligned(8)));

struct test_memory_map_tag {
    struct multiboot2_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot2_memory_map_entry entry;
} __attribute__((packed));

struct test_memory_information {
    struct multiboot2_information_header header;
    struct test_memory_map_tag memory_map;
    struct multiboot2_tag end;
} __attribute__((packed, aligned(8)));

/*
 * A framebuffer tag, built at runtime so each field can be broken one at a
 * time. It has to be mutable and it has to carry a memory map beside it,
 * because the parser refuses information with no memory map before it ever
 * looks at anything else.
 *
 * This fixture exists because the tested machine cannot drive these refusals.
 * QEMU reports a pitch exactly equal to a row, a depth of exactly 32 and three
 * byte-aligned channels, so every rejection below is unreachable from hardware
 * - which was discovered by deleting the pitch check and watching the suite
 * stay green.
 */
#define TEST_FRAMEBUFFER_TAG_SIZE 38U

struct test_framebuffer_tag {
    struct multiboot2_tag tag;
    uint8_t body[TEST_FRAMEBUFFER_TAG_SIZE - sizeof(struct multiboot2_tag)];
    uint8_t padding[2];
} __attribute__((packed));

struct framebuffer_information {
    struct multiboot2_information_header header;
    struct test_memory_map_tag memory_map;
    struct test_framebuffer_tag framebuffer;
    struct multiboot2_tag end;
} __attribute__((packed, aligned(8)));

struct two_framebuffer_information {
    struct multiboot2_information_header header;
    struct test_memory_map_tag memory_map;
    struct test_framebuffer_tag first;
    struct test_framebuffer_tag second;
    struct multiboot2_tag end;
} __attribute__((packed, aligned(8)));

/* Offsets inside the tag body, counted from the start of the tag. */
#define FB_OFFSET_ADDRESS 8U
#define FB_OFFSET_PITCH 16U
#define FB_OFFSET_WIDTH 20U
#define FB_OFFSET_HEIGHT 24U
#define FB_OFFSET_DEPTH 28U
#define FB_OFFSET_KIND 29U
#define FB_OFFSET_RED_POSITION 32U
#define FB_OFFSET_RED_SIZE 33U
#define FB_OFFSET_GREEN_POSITION 34U
#define FB_OFFSET_GREEN_SIZE 35U
#define FB_OFFSET_BLUE_POSITION 36U
#define FB_OFFSET_BLUE_SIZE 37U

/*
 * Deliberately padded: the pitch exceeds the width in bytes, which is the case
 * no machine here produces and the one an addressing mistake shows up in.
 */
#define TEST_FB_ADDRESS UINT64_C(0x00100000)
#define TEST_FB_PITCH 512U
#define TEST_FB_WIDTH 100U
#define TEST_FB_HEIGHT 50U

static const struct empty_information empty_information = {
    .header = {
        .total_size = sizeof(struct empty_information),
        .reserved = 0
    },
    .end = {
        .type = MULTIBOOT2_TAG_END,
        .size = sizeof(struct multiboot2_tag)
    }
};

static const struct bad_end_information bad_end_information = {
    .header = {
        .total_size = sizeof(struct bad_end_information),
        .reserved = 0
    },
    .end = {
        .type = MULTIBOOT2_TAG_END,
        .size = sizeof(struct multiboot2_tag)
    },
    .unexpected_tail = 0
};

static const struct unterminated_string_information unterminated_information = {
    .header = {
        .total_size = sizeof(struct unterminated_string_information),
        .reserved = 0
    },
    .string = {
        .tag = {
            .type = MULTIBOOT2_TAG_COMMAND_LINE,
            .size = sizeof(struct multiboot2_tag) + 4U
        },
        .bytes = {'B', 'A', 'D', '!'},
        .padding = 0
    },
    .end = {
        .type = MULTIBOOT2_TAG_END,
        .size = sizeof(struct multiboot2_tag)
    }
};

static const struct module_information module_information = {
    .header = {
        .total_size = sizeof(struct module_information),
        .reserved = 0
    },
    .module = {
        .tag = {
            .type = MULTIBOOT2_TAG_MODULE,
            .size = sizeof(struct multiboot2_tag) + 2U * sizeof(uint32_t) + 1U
        },
        .module_start = UINT32_C(0x200000),
        .module_end = UINT32_C(0x210000),
        .name = {'\0'},
        .padding = {0, 0, 0, 0, 0, 0, 0}
    },
    .end = {
        .type = MULTIBOOT2_TAG_END,
        .size = sizeof(struct multiboot2_tag)
    }
};

static const struct test_memory_information overflowing_memory_information = {
    .header = {
        .total_size = sizeof(struct test_memory_information),
        .reserved = 0
    },
    .memory_map = {
        .tag = {
            .type = MULTIBOOT2_TAG_MEMORY_MAP,
            .size = sizeof(struct test_memory_map_tag)
        },
        .entry_size = sizeof(struct multiboot2_memory_map_entry),
        .entry_version = 0,
        .entry = {
            .base_address = UINT64_MAX - 1U,
            .length = 4,
            .type = MULTIBOOT2_MEMORY_AVAILABLE,
            .reserved = 0
        }
    },
    .end = {
        .type = MULTIBOOT2_TAG_END,
        .size = sizeof(struct multiboot2_tag)
    }
};

static const struct test_memory_information valid_memory_information = {
    .header = {
        .total_size = sizeof(struct test_memory_information),
        .reserved = 0
    },
    .memory_map = {
        .tag = {
            .type = MULTIBOOT2_TAG_MEMORY_MAP,
            .size = sizeof(struct test_memory_map_tag)
        },
        .entry_size = sizeof(struct multiboot2_memory_map_entry),
        .entry_version = 0,
        .entry = {
            .base_address = UINT64_C(0x100000),
            .length = UINT64_C(0x1000000),
            .type = MULTIBOOT2_MEMORY_AVAILABLE,
            .reserved = 0
        }
    },
    .end = {
        .type = MULTIBOOT2_TAG_END,
        .size = sizeof(struct multiboot2_tag)
    }
};

static uint8_t *framebuffer_field(struct test_framebuffer_tag *tag,
    unsigned int offset)
{
    return (uint8_t *)(void *)tag + offset;
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    for (unsigned int index = 0; index < 4U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void write_le64(uint8_t *bytes, uint64_t value)
{
    for (unsigned int index = 0; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void prepare_framebuffer_tag(struct test_framebuffer_tag *tag)
{
    for (unsigned int index = 0; index < sizeof(*tag); ++index) {
        ((uint8_t *)(void *)tag)[index] = 0U;
    }

    tag->tag.type = MULTIBOOT2_TAG_FRAMEBUFFER;
    tag->tag.size = TEST_FRAMEBUFFER_TAG_SIZE;
    write_le64(framebuffer_field(tag, FB_OFFSET_ADDRESS), TEST_FB_ADDRESS);
    write_le32(framebuffer_field(tag, FB_OFFSET_PITCH), TEST_FB_PITCH);
    write_le32(framebuffer_field(tag, FB_OFFSET_WIDTH), TEST_FB_WIDTH);
    write_le32(framebuffer_field(tag, FB_OFFSET_HEIGHT), TEST_FB_HEIGHT);
    *framebuffer_field(tag, FB_OFFSET_DEPTH) =
        BOOT_FRAMEBUFFER_BITS_PER_PIXEL;
    *framebuffer_field(tag, FB_OFFSET_KIND) = MULTIBOOT2_FRAMEBUFFER_TYPE_RGB;
    *framebuffer_field(tag, FB_OFFSET_RED_POSITION) = 16U;
    *framebuffer_field(tag, FB_OFFSET_RED_SIZE) = 8U;
    *framebuffer_field(tag, FB_OFFSET_GREEN_POSITION) = 8U;
    *framebuffer_field(tag, FB_OFFSET_GREEN_SIZE) = 8U;
    *framebuffer_field(tag, FB_OFFSET_BLUE_POSITION) = 0U;
    *framebuffer_field(tag, FB_OFFSET_BLUE_SIZE) = 8U;
}

static void prepare_framebuffer_fixture(struct framebuffer_information *fixture)
{
    fixture->header.total_size = sizeof(*fixture);
    fixture->header.reserved = 0U;
    fixture->memory_map.tag.type = MULTIBOOT2_TAG_MEMORY_MAP;
    fixture->memory_map.tag.size = sizeof(struct test_memory_map_tag);
    fixture->memory_map.entry_size =
        sizeof(struct multiboot2_memory_map_entry);
    fixture->memory_map.entry_version = 0U;
    fixture->memory_map.entry.base_address = UINT64_C(0x100000);
    fixture->memory_map.entry.length = UINT64_C(0x1000000);
    fixture->memory_map.entry.type = MULTIBOOT2_MEMORY_AVAILABLE;
    fixture->memory_map.entry.reserved = 0U;
    prepare_framebuffer_tag(&fixture->framebuffer);
    fixture->end.type = MULTIBOOT2_TAG_END;
    fixture->end.size = sizeof(struct multiboot2_tag);
}

static enum boot_status parse_framebuffer_fixture(
    const struct framebuffer_information *fixture,
    struct boot_context *context
)
{
    return boot_context_parse(
        MULTIBOOT2_BOOT_MAGIC,
        (uintptr_t)(const void *)fixture,
        context
    );
}

/*
 * Every framebuffer refusal, driven by breaking one field of a fixture that is
 * otherwise accepted, so each check is reached with everything before it valid.
 */
static bool framebuffer_rejections_are_named(void)
{
    struct framebuffer_information fixture;
    struct two_framebuffer_information duplicate;
    struct boot_context context;

    /* The acceptance case, which is what makes the rejections mean anything. */
    prepare_framebuffer_fixture(&fixture);

    if (parse_framebuffer_fixture(&fixture, &context) != BOOT_STATUS_OK ||
        !context.framebuffer.present ||
        context.framebuffer.address != TEST_FB_ADDRESS ||
        context.framebuffer.pitch != TEST_FB_PITCH ||
        context.framebuffer.width != TEST_FB_WIDTH ||
        context.framebuffer.height != TEST_FB_HEIGHT ||
        context.framebuffer.size !=
            (uint64_t)TEST_FB_PITCH * TEST_FB_HEIGHT ||
        context.framebuffer.red_position != 16U ||
        context.framebuffer.green_position != 8U ||
        context.framebuffer.blue_position != 0U) {
        return false;
    }

    /* An indexed framebuffer needs a palette this kernel does not read. */
    prepare_framebuffer_fixture(&fixture);
    *framebuffer_field(&fixture.framebuffer, FB_OFFSET_KIND) =
        (uint8_t)MULTIBOOT2_FRAMEBUFFER_TYPE_INDEXED;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_FRAMEBUFFER_NOT_DIRECT_COLOUR) {
        return false;
    }

    /* EGA text is not a framebuffer at all. */
    prepare_framebuffer_fixture(&fixture);
    *framebuffer_field(&fixture.framebuffer, FB_OFFSET_KIND) =
        (uint8_t)MULTIBOOT2_FRAMEBUFFER_TYPE_EGA_TEXT;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_FRAMEBUFFER_NOT_DIRECT_COLOUR) {
        return false;
    }

    prepare_framebuffer_fixture(&fixture);
    *framebuffer_field(&fixture.framebuffer, FB_OFFSET_DEPTH) = 24U;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_DEPTH) {
        return false;
    }

    prepare_framebuffer_fixture(&fixture);
    write_le32(framebuffer_field(&fixture.framebuffer, FB_OFFSET_WIDTH), 0U);

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_GEOMETRY) {
        return false;
    }

    prepare_framebuffer_fixture(&fixture);
    write_le32(framebuffer_field(&fixture.framebuffer, FB_OFFSET_HEIGHT), 0U);

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_GEOMETRY) {
        return false;
    }

    /*
     * One pixel short of covering a row. This is the refusal no machine here
     * can reach, and the one whose absence shears every picture drawn on it.
     */
    prepare_framebuffer_fixture(&fixture);
    write_le32(
        framebuffer_field(&fixture.framebuffer, FB_OFFSET_PITCH),
        TEST_FB_WIDTH * BOOT_FRAMEBUFFER_BYTES_PER_PIXEL - 4U
    );

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_PITCH) {
        return false;
    }

    /* A pitch that is not a whole number of pixels. */
    prepare_framebuffer_fixture(&fixture);
    write_le32(
        framebuffer_field(&fixture.framebuffer, FB_OFFSET_PITCH),
        TEST_FB_PITCH + 1U
    );

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_PITCH) {
        return false;
    }

    prepare_framebuffer_fixture(&fixture);
    write_le64(framebuffer_field(&fixture.framebuffer, FB_OFFSET_ADDRESS), 0U);

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_ADDRESS) {
        return false;
    }

    prepare_framebuffer_fixture(&fixture);
    write_le64(
        framebuffer_field(&fixture.framebuffer, FB_OFFSET_ADDRESS),
        TEST_FB_ADDRESS + 1U
    );

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_ADDRESS) {
        return false;
    }

    /* A framebuffer whose last row falls off the end of the early map. */
    prepare_framebuffer_fixture(&fixture);
    write_le64(
        framebuffer_field(&fixture.framebuffer, FB_OFFSET_ADDRESS),
        SENERI_EARLY_PHYSICAL_LIMIT -
            (uint64_t)TEST_FB_PITCH * TEST_FB_HEIGHT + 4U
    );

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_FRAMEBUFFER_OUTSIDE_EARLY_MAP) {
        return false;
    }

    /* A channel narrower than a byte. */
    prepare_framebuffer_fixture(&fixture);
    *framebuffer_field(&fixture.framebuffer, FB_OFFSET_RED_SIZE) = 5U;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_CHANNEL) {
        return false;
    }

    /* A channel that does not start on a byte boundary. */
    prepare_framebuffer_fixture(&fixture);
    *framebuffer_field(&fixture.framebuffer, FB_OFFSET_GREEN_POSITION) = 3U;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_CHANNEL) {
        return false;
    }

    /* A channel past the end of a pixel. */
    prepare_framebuffer_fixture(&fixture);
    *framebuffer_field(&fixture.framebuffer, FB_OFFSET_BLUE_POSITION) = 32U;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_CHANNEL) {
        return false;
    }

    /* Two channels on the same bits, which would make one overwrite another. */
    prepare_framebuffer_fixture(&fixture);
    *framebuffer_field(&fixture.framebuffer, FB_OFFSET_BLUE_POSITION) = 16U;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_BAD_FRAMEBUFFER_CHANNEL) {
        return false;
    }

    /* A tag too short to hold the fields the parser is about to read. */
    prepare_framebuffer_fixture(&fixture);
    fixture.framebuffer.tag.size = MULTIBOOT2_FRAMEBUFFER_COMMON_SIZE - 1U;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_FRAMEBUFFER_TAG_TOO_SMALL) {
        return false;
    }

    /* Long enough for the common part, too short for the colour description. */
    prepare_framebuffer_fixture(&fixture);
    fixture.framebuffer.tag.size = MULTIBOOT2_FRAMEBUFFER_COMMON_SIZE;

    if (parse_framebuffer_fixture(&fixture, &context) !=
        BOOT_STATUS_FRAMEBUFFER_TAG_TOO_SMALL) {
        return false;
    }

    /* Two framebuffers cannot both be the screen. */
    duplicate.header.total_size = sizeof(duplicate);
    duplicate.header.reserved = 0U;
    prepare_framebuffer_fixture(&fixture);
    duplicate.memory_map = fixture.memory_map;
    prepare_framebuffer_tag(&duplicate.first);
    prepare_framebuffer_tag(&duplicate.second);
    duplicate.end.type = MULTIBOOT2_TAG_END;
    duplicate.end.size = sizeof(struct multiboot2_tag);

    return boot_context_parse(
        MULTIBOOT2_BOOT_MAGIC,
        (uintptr_t)(const void *)&duplicate,
        &context
    ) == BOOT_STATUS_DUPLICATE_FRAMEBUFFER;
}

bool boot_parser_self_test(void)
{
    struct boot_context context;

    if (boot_context_parse(0U, 0U, &context) != BOOT_STATUS_BAD_MAGIC) {
        return false;
    }

    if (boot_context_parse(MULTIBOOT2_BOOT_MAGIC, 0U, &context) !=
        BOOT_STATUS_NULL_INFORMATION) {
        return false;
    }

    if (boot_context_parse(
            MULTIBOOT2_BOOT_MAGIC,
            (uintptr_t)(const void *)&empty_information + 1U,
            &context
        ) != BOOT_STATUS_MISALIGNED_INFORMATION) {
        return false;
    }

    if (boot_context_parse(
            MULTIBOOT2_BOOT_MAGIC,
            (uintptr_t)(const void *)&empty_information,
            &context
        ) != BOOT_STATUS_MISSING_MEMORY_MAP) {
        return false;
    }

    if (boot_context_parse(
            MULTIBOOT2_BOOT_MAGIC,
            (uintptr_t)(const void *)&bad_end_information,
            &context
        ) != BOOT_STATUS_BAD_END_TAG) {
        return false;
    }

    if (boot_context_parse(
            MULTIBOOT2_BOOT_MAGIC,
            (uintptr_t)(const void *)&unterminated_information,
            &context
        ) != BOOT_STATUS_STRING_NOT_TERMINATED) {
        return false;
    }

    if (boot_context_parse(
            MULTIBOOT2_BOOT_MAGIC,
            (uintptr_t)(const void *)&overflowing_memory_information,
            &context
        ) != BOOT_STATUS_MEMORY_REGION_OVERFLOW) {
        return false;
    }

    if (boot_context_parse(
            MULTIBOOT2_BOOT_MAGIC,
            (uintptr_t)(const void *)&module_information,
            &context
        ) != BOOT_STATUS_UNSUPPORTED_MODULE) {
        return false;
    }

    if (boot_context_parse(
            MULTIBOOT2_BOOT_MAGIC,
            (uintptr_t)(const void *)&valid_memory_information,
            &context
        ) != BOOT_STATUS_OK) {
        return false;
    }

    if (context.memory_map_entry_count != 1U ||
        context.reported_usable_bytes != UINT64_C(0x1000000) ||
        context.highest_reported_address != UINT64_C(0x1100000)) {
        return false;
    }

    /*
     * Information with no framebuffer tag must leave the context saying so
     * rather than leaving it uninitialised. Everything above this line has been
     * parsing exactly that, so it is asserted here.
     */
    if (context.framebuffer.present) {
        return false;
    }

    return framebuffer_rejections_are_named();
}
