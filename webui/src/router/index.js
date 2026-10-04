import { createRouter, createWebHashHistory } from "vue-router";

// pywebview 以 file:// 加载构建产物，HTML5 history 模式会抛 SecurityError，
// 必须使用 hash 模式（如 #/main、#/log）才能兼容。
const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    {
      path: "/",
      redirect: "/main",
    },
    {
      path: "/main",
      name: "main",
      components: {
        screen: () => import("../components/Screen/Main.vue"),
      },
    },
    {
      path: "/log",
      name: "log",
      components: {
        screen: () => import("../components/Screen/Log.vue"),
      },
    },
    {
      path: "/:pathMatch(.*)*",
      redirect: "/main",
    },
  ],
});

export default router;
