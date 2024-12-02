pub(crate) use ot_sw_device_silicon_creator_lib_drivers_rnd::rnd_uint32;
pub(crate) use ot_sw_device_silicon_creator_lib_drivers_rnd_automock::rnd_uint32_context;

#[test]
fn test_launder32_same() {
    let ctx = rnd_uint32_context();
    ctx.expect().returning(|| 42);
    // Safety: testing
    assert_eq!(42, unsafe { rnd_uint32() });
}
