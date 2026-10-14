import { base, shots } from "./lib.mjs";
export default async function (b) {
  await b.viewport(1440, 900);
  for (const hash of (process.env.ROUTES || "devices").split(",")) {
    await b.goto(base + "#" + hash, 2000);
    console.log(hash, JSON.stringify(await b.eval("document.querySelector('#view').innerText.slice(0, 160)")));
    await b.shot(`${shots}/smoke-${hash.replace(/\W+/g, "_").slice(0, 40)}.png`);
  }
}
