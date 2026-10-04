import { createRouter, createWebHistory } from "vue-router";

const router = createRouter({
  history: createWebHistory(import.meta.env.BASE_URL),
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
  ],
});

export default router;
