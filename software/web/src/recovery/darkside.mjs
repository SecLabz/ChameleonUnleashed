export async function recoverDarkside({ device, target, geometry, call, verify, check, update }) {
  try {
    for (let variant = 0; variant < 30; variant++) {
      check();

      update({ phase: `Darkside · sector ${target.sector} · capture ${variant + 1}` });
      const observation = await device.captureDarkside(target, variant, geometry);

      if (!observation) {
        return 'NoVerifiedKey';
      }

      const { candidates } = await call({ op: 'darkside', observation });
      const key = await verify(target, candidates);

      if (key) {
        return { Verified: key };
      }
    }

    return 'NoVerifiedKey';
  } catch (error) {
    if (!/ResourceLimit/.test(error.message) && error.httpStatus !== 404) {
      throw error;
    }

    let message = error.message;

    if (error.httpStatus === 404) {
      message = 'This firmware does not support web Darkside; update the application firmware.';
    }

    update({ phase: message });

    return 'ResourceLimited';
  }
}
