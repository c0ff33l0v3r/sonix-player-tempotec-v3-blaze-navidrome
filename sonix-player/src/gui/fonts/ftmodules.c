// FreeType's start-up, with only the modules the player's fonts need.
//
// FT_Init_FreeType() in FreeType's ftinit.c registers every module the
// library was configured with -- the drivers for Type 1, CID, Type 42, PFR,
// Windows FNT, PCF and BDF, the gzip and LZW readers PCF and BDF bring along,
// the monochrome and SDF renderers. Each sits in a table there, so
// --gc-sections can drop none of them, and FreeType is linked statically (see
// the Makefile): some 125 kB of code that is pinned in RAM with the rest of
// the player (lock_code_pages in main.c) and never runs. The interface, the
// e-book reader and the charge screen only ever open .ttf and .otf files
// (src/gui/fonts/fonts.c, the e-book font list, chargescreen.c), so:
//
//   truetype, sfnt      TrueType outlines and the OpenType tables around them
//   cff, psaux, psnames .otf, which carries CFF outlines
//   pshinter            CFF's hinter
//   autofit             kept: FT_LOAD_DEFAULT, the charge screen's flags, may
//                       still ask for it, and without it those glyphs would
//                       come out hinted differently
//   smooth              the anti-aliased renderer, the only mode drawn
//   svg                 kept with FT_LOAD_COLOR in LVGL's binding: 1 kB, and
//                       an SVG glyph then fails the same way as before
//
// in the order ftinit.c gives them, which is the order FT_Open_Face tries the
// drivers in. A font in any other format now fails FT_New_Face with
// Unknown_File_Format, as a corrupt file would.
//
// Defining these four here keeps ftinit.o, which holds all four and the table,
// out of the link: all four, or the linker pulls ftinit.o in for the missing
// one and then finds the others defined twice. FT_Set_Default_Properties only
// reads the FREETYPE_PROPERTIES variable, which nothing on the device sets.
//
// Target only: the host links the system libfreetype.so, which does not export
// the module classes.
#ifndef HOST_BUILD

#include <stdlib.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_RENDER_H
#include FT_SYSTEM_H

// A driver class is an FT_Driver_ClassRec, an internal type that starts with
// its FT_Module_Class; only the address is taken, which is all FT_Add_Module
// wants, so the public type does for the declaration.
extern const FT_Module_Class tt_driver_class;
extern const FT_Module_Class cff_driver_class;
extern const FT_Module_Class sfnt_module_class;
extern const FT_Module_Class autofit_module_class;
extern const FT_Module_Class pshinter_module_class;
extern const FT_Renderer_Class ft_smooth_renderer_class;
extern const FT_Renderer_Class ft_svg_renderer_class;
extern const FT_Module_Class psaux_module_class;
extern const FT_Module_Class psnames_module_class;

static const FT_Module_Class *const modules[] = {
	&tt_driver_class,
	&cff_driver_class,
	&sfnt_module_class,
	&autofit_module_class,
	&pshinter_module_class,
	(const FT_Module_Class *)&ft_smooth_renderer_class,
	(const FT_Module_Class *)&ft_svg_renderer_class,
	&psaux_module_class,
	&psnames_module_class,
};

// What ftsystem.c's FT_New_Memory() hands out, minus the allocation: it holds
// no state, so one shared by every library does, and FT_Done_FreeType has
// nothing to free.
static void *mem_alloc(FT_Memory memory, long size) {
	(void)memory;
	return malloc((size_t)size);
}

static void mem_free(FT_Memory memory, void *block) {
	(void)memory;
	free(block);
}

static void *mem_realloc(FT_Memory memory, long cur_size, long new_size, void *block) {
	(void)memory;
	(void)cur_size;
	return realloc(block, (size_t)new_size);
}

static struct FT_MemoryRec_ memory_rec = {NULL, mem_alloc, mem_free, mem_realloc};

void FT_Add_Default_Modules(FT_Library library) {
	for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
		FT_Add_Module(library, modules[i]);
	}
}

void FT_Set_Default_Properties(FT_Library library) { (void)library; }

FT_Error FT_Init_FreeType(FT_Library *alibrary) {
	FT_Error error = FT_New_Library(&memory_rec, alibrary);
	if (error == 0) {
		FT_Add_Default_Modules(*alibrary);
	}
	return error;
}

FT_Error FT_Done_FreeType(FT_Library library) { return FT_Done_Library(library); }

#endif
