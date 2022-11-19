#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../format.h"
#include "../filemap.h"

enum ips_error
{
    IPS_SUCCESS = 0,
    IPS_INVALID_HEADER,
    IPS_TOO_SMALL,
    IPS_NO_FOOTER,
    IPS_ERROR_COUNT
};

static const char *ips_error_messages[IPS_ERROR_COUNT];
static int ips_patch(patch_context_t *);

static const char *ips_error_messages[IPS_ERROR_COUNT] = {
    [IPS_SUCCESS] = "IPS patching successful.",
    [IPS_INVALID_HEADER] = "Invalid header for an IPS file.",
    [IPS_TOO_SMALL] = "Patch file is too small to be an IPS file.",
    [IPS_NO_FOOTER] = "EOF footer not found."
};

const patch_format_t ips_format = { "IPS", "PATCH", 5, ips_patch, ips_error_messages };

static int ips_patch(patch_context_t *c)
{
    uint8_t *patch, *patchend, *input, *output;

    if (c->patch.status == -1)
        return ERROR_PATCH_FILE_MMAP;

    if (c->patch.size < 8)
        return IPS_TOO_SMALL;

    patch = c->patch.handle;
    patchend = patch + c->patch.size;

#define patch8() ((patch < patchend) ? *(patch++) : 0)
#define patch16() ((patch + 2 < patchend) ? (patch += 2, (patch[-2] << 8 | patch[-1])) : 0)
#define patch24() ((patch + 3 < patchend) ? (patch += 3, (patch[-3] << 16 | patch[-2] << 8 | patch[-1])) : 0)

    if (patch8() != 'P' || patch8() != 'A' || patch8() != 'T' || patch8() != 'C' || patch8() != 'H')
        return IPS_INVALID_HEADER; // Never gonna get called, unless the function gets used directly.

    c->input = mmap_file_new(c->fn.input, 1);
    mmap_open(&c->input);

    if (c->input.status == -1)
        return ERROR_INPUT_FILE_MMAP;

    input = c->input.handle;

    c->output = mmap_file_new(c->fn.output, 0);
    mmap_create(&c->output, c->input.size);

    if (!c->output.status)
        return ERROR_OUTPUT_FILE_MMAP;

    output = c->output.handle;

    memcpy(output, input, c->output.size);

    mmap_close(&c->input);

    while (patch < patchend - 3)
    {
        uint32_t offset = patch24();
        uint16_t size = patch16();

        uint8_t *outputoff = (output + offset);

        if (size)
        {
            while (size--)
                *(outputoff++) = patch8();
        }
        else
        {
            size = patch16();
            uint8_t byte = patch8();

            while (size--)
                *(outputoff++) = byte;
        }
    }

    if (patch8() != 'E' || patch8() != 'O' || patch8() != 'F')
        return IPS_NO_FOOTER;

#undef patch8
#undef patch16
#undef patch24

    return IPS_SUCCESS;
}

