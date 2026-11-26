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
      path: "/settings/account",
      name: "account",
      components: {
        screen: () => import("../components/Screen/Settings/Account.vue"),
      },
    },
    {
      path: "/settings/login_info",
      name: "login_info",
      components: {
        screen: () => import("../components/Screen/Settings/LoginInfo.vue"),
      },
    },
    {
      path: "/settings/server",
      name: "server",
      components: {
        screen: () => import("../components/Screen/Settings/Server.vue"),
      },
    },
    {
      path: "/settings/language",
      name: "language",
      components: {
        screen: () => import("../components/Screen/Settings/Language.vue"),
      },
    },
    {
      path: "/settings/cabin",
      name: "cabin",
      components: {
        screen: () => import("../components/Screen/Settings/Cabin.vue"),
      },
    },
    {
      path: "/settings",
      name: "settings",
      components: {
        screen: () => import("../components/Screen/Settings.vue"),
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
      path: "/atc",
      name: "atc",
      components: {
        screen: () => import("../components/Screen/Online/AtcList.vue"),
      },
    },
    {
      path: "/pilots",
      name: "pilots",
      components: {
        screen: () => import("../components/Screen/Online/PilotList.vue"),
      },
    },
    {
      path: "/:pathMatch(.*)*",
      redirect: "/main",
    },
  ],
});

export default router;
