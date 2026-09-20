// Host tool: prints the input usage slots of a GNM shader binary (.sb), i.e. which user
// SGPRs the shader expects its resource pointers in.
// Build: gcc -std=gnu11 -I<opengnm>/include ps4/tools/sbinfo.c -o sbinfo
// Usage: sbinfo <vs|ps> <file.sb>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gnm/pssl/types.h>
#include <gnm_shader.h>
#include <gnm_shaderbinary.h>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <vs|ps> <file.sb>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f)
    {
        perror(argv[2]);
        return 1;
    }
    static unsigned char buf[1 << 20];
    size_t size = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    const GnmShaderFileHeader *hdr = (const GnmShaderFileHeader *)(buf + sizeof(PsslBinaryHeader));
    const GnmShaderCommonData *common = (const GnmShaderCommonData *)((const unsigned char *)hdr + sizeof(*hdr));
    printf("%s: %zu bytes, magic %s, %u input usage slot(s)\n", argv[2], size,
           hdr->magic == GNM_SHADER_FILE_HEADER_ID ? "ok" : "BAD", common->numinputusageslots);

    const GnmInputUsageSlot *slots = strcmp(argv[1], "vs") == 0
                                         ? sceGnmVsShaderInputUsageSlotTable((const GnmVsShader *)common)
                                         : sceGnmPsShaderInputUsageSlotTable((const GnmPsShader *)common);
    for (unsigned i = 0; i < common->numinputusageslots; i++)
    {
        printf("  slot %u: usagetype 0x%02x apislot %u startregister %u\n", i, slots[i].usagetype, slots[i].apislot,
               slots[i].startregister);
    }

    // Vertex shaders also say which vertex buffer slot feeds each attribute, and in what
    // order: that mapping is what a fetch shader is built from.
    if (strcmp(argv[1], "vs") == 0)
    {
        const GnmVsShader *vs = (const GnmVsShader *)common;
        const GnmVertexInputSemantic *inputs = sceGnmVsShaderInputSemanticTable(vs);
        printf("  %u vertex input semantic(s):\n", vs->numinputsemantics);
        for (unsigned i = 0; i < vs->numinputsemantics; i++)
        {
            printf("    input %u: semantic %u -> vgpr %u, %u element(s)\n", i, inputs[i].semantic,
                   inputs[i].vgpr, inputs[i].sizeinelements);
        }
        const GnmVertexExportSemantic *exports = sceGnmVsShaderExportSemanticTable(vs);
        printf("  %u export semantic(s):\n", vs->numexportsemantics);
        for (unsigned i = 0; i < vs->numexportsemantics; i++)
        {
            printf("    export %u: semantic %u\n", i, exports[i].semantic);
        }
    }
    // Pixel shaders list the interpolants they expect; these must line up with the vertex
    // shader exports above, or the pixel shader reads the wrong varying.
    if (strcmp(argv[1], "ps") == 0)
    {
        const GnmPsShader *ps = (const GnmPsShader *)common;
        const GnmPixelInputSemantic *inputs = sceGnmPsShaderInputSemanticTable(ps);
        printf("  %u pixel input semantic(s):\n", ps->numinputsemantics);
        for (unsigned i = 0; i < ps->numinputsemantics; i++)
        {
            printf("    input %u: semantic %u, default %u, flat %u\n", i, inputs[i].semantic,
                   inputs[i].defaultvalue, inputs[i].isflatshaded);
        }
    }
    return 0;
}
