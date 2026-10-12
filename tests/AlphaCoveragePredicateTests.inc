// Include in the existing test namespace after HighSliceHtileRegistrationNeighbors.
// Post-core neighbor of the shared requested-coverage predicate; GPU32/64 remains
// the independent behavior proof. This CPU check deliberately needs the new API.
void CheckAlphaCoveragePredicateNeighbors() {
  constexpr const char* name="AlphaCoveragePredicateNeighbors";
  struct Format {u32 mode;bool has_noninteger_alpha;};
  // Independent AMD COL0 export-format capability table, not a sampled value
  // from the production predicate. Modes7/8 are integer;0/1/2 have no alpha.
  constexpr std::array formats{Format{0u,false},Format{1u,false},Format{2u,false},
      Format{3u,true},Format{4u,true},Format{5u,true},Format{6u,true},
      Format{7u,false},Format{8u,false},Format{9u,true}};
  u32 checked=0u;
  for(const auto& format:formats)for(bool enabled:{false,true})for(bool override_off:{false,true}) {
    HW::Context context{};context.SetTargetOutputMode(0u,format.mode);
    context.SetTargetOutputMode(1u,9u); // later floating MRT cannot supply COL0alpha
    const u32 raw=0x0000aa00u|(enabled?1u:0u);
    context.SetAlphaToMaskRaw(raw);
    context.SetDepthShaderControl({.alpha_to_mask_disable=override_off});
    const bool expected=format.has_noninteger_alpha&&enabled&&!override_off;
    Require(name,"COL0 format/enable/override contract",
        context.AlphaToCoverageEnabled()==expected&&context.GetAlphaToMaskRaw()==raw,
        "coverage requested from no-alpha/integer/laterMRT, lost floatingCOL0, or ignoredoverride");
    ++checked;
  }
  // Every documented quad pattern is retained. This is an endpoint/request
  // invariant, not proof of any fractional threshold or host dither equivalence.
  constexpr std::array<u32,5> patterns{0x00000000u,0x00005500u,0x0000aa00u,
                                      0x0000ff00u,0x00018700u}; //3102+round1
  for(u32 pattern:patterns)for(bool enabled:{false,true})for(bool override_off:{false,true}) {
    HW::Context context{};context.SetTargetOutputMode(0u,9u);
    const u32 raw=pattern|(enabled?1u:0u);context.SetAlphaToMaskRaw(raw);
    context.SetDepthShaderControl({.alpha_to_mask_disable=override_off});
    Require(name,"raw quad pattern preservation and independent override",
        context.GetAlphaToMaskRaw()==raw&&context.AlphaToCoverageEnabled()==(enabled&&!override_off),
        "valid offset/round pattern was discarded or changed basic enabled state");
    ++checked;
  }
  HW::Context reset{};reset.SetTargetOutputMode(0u,9u);reset.SetAlphaToMaskRaw(0x18701u);
  Require(name,"initial reset control",reset.AlphaToCoverageEnabled(),"floating enabled setup failed");
  reset.Reset();
  Require(name,"context reset clears requested coverage",!reset.AlphaToCoverageEnabled()&&
      reset.GetAlphaToMaskRaw()==0u,"old coverage state escaped a context reset");
  std::printf("[host] AlphaCoveragePredicateNeighbors %u format/override/raw cases and reset PASS\n",checked);
}
