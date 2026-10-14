// 多宽度截图与横向溢出检查。ROUTES 以逗号分隔 hash；输出到 shots/<宽度>-<路由>.png。
import { base, shots, check, summary } from "./lib.mjs";
export default async function (b) {
  const routes = (process.env.ROUTES || "devices").split(",");
  for (const [width, height] of [[1920, 1080], [1440, 900], [1024, 768], [390, 844]]) {
    await b.viewport(width, height);
    await b.goto(base + "#" + routes[0], 2200);
    for (const route of routes) {
      await b.eval(`location.hash = ${JSON.stringify("#" + route)}`);
      await b.sleep(1200);
      const overflow = await b.eval("document.documentElement.scrollWidth - innerWidth");
      check(`${width} ${route} 无横向溢出`, overflow <= 0, String(overflow));
      await b.shot(`${shots}/${width}-${route.split("/").slice(0, 2).join("-")}.png`, width === 390);
    }
  }
  summary();
}
