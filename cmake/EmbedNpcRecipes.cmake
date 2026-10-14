# Writes a C++ source with every NPC catalogue recipe (runtime/assets/npc_catalogue/*.recipe.json) as text:
#   cmake -DDIR=dir -DOUT=out.cpp -P EmbedNpcRecipes.cmake
# defines `const char* const wwhd_npc_recipes[]` and `const unsigned wwhd_npc_recipe_count`
# (story_npc_catalogue.h). A recipe holds our own words and references into the game only; names and
# spots come from the player's game (tools/story_mods/npc_catalogue.py). A new place is a new recipe.
file(GLOB tables "${DIR}/*.recipe.json")
list(SORT tables)
set(body "")
set(count 0)
foreach(table ${tables})
  file(READ "${table}" text)
  string(FIND "${text}" ")wwhd\"" clash)
  if(NOT clash EQUAL -1)
    message(FATAL_ERROR "${table} contains the raw string delimiter")
  endif()
  get_filename_component(name "${table}" NAME)
  string(APPEND body "    // ${name}\n    R\"wwhd(${text})wwhd\",\n")
  math(EXPR count "${count} + 1")
endforeach()
if(count EQUAL 0)
  set(body "    \"\",\n")
endif()
file(WRITE "${OUT}.tmp" "// generated from ${DIR} by cmake/EmbedNpcRecipes.cmake\nextern \"C\" const char* const wwhd_npc_recipes[] = {\n${body}};\nextern \"C\" const unsigned wwhd_npc_recipe_count = ${count};\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
