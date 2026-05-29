// Linux backend: GLFW + OpenGL3, mirroring the Win32+DX11 backend in main.cpp.
// All UI and logic live in the shared App core (app.cpp); this file only owns the
// window, the GL context, and the frame loop.
#include <cstdio>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include "app.h"
#include "platform.h"

static App* g_app = nullptr;

static void renderFrame(GLFWwindow* win);

static void glfwErrorCb(int err,const char* desc) {
	fprintf(stderr,"[GLFW] error %d: %s\n",err,desc);
}

// Keep rendering while the window is being resized/moved (analogous to the Win32
// WM_TIMER trick): GLFW fires this callback during the modal resize loop.
static void windowRefreshCb(GLFWwindow* win) {
	if (g_app) renderFrame(win);
}

static void renderFrame(GLFWwindow* win) {
	g_app->update();

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	g_app->drawUI();

	ImGui::Render();
	int w,h;
	glfwGetFramebufferSize(win,&w,&h);
	glViewport(0,0,w,h);
	glClearColor(0.08f,0.08f,0.08f,1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	glfwSwapBuffers(win);
}

int main(int,char**) {
	plat::raisePrivileges();

	glfwSetErrorCallback(glfwErrorCb);
	if (!glfwInit()) {
		fprintf(stderr,"[!] glfwInit failed\n");
		return 1;
	}

	// GL 3.0 + GLSL 130 — matches imgui_impl_opengl3's default and works on the
	// widest range of Linux GPU drivers.
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,0);

	GLFWwindow* win = glfwCreateWindow(820,660,"FRS - Free RAM System",nullptr,nullptr);
	if (!win) {
		fprintf(stderr,"[!] glfwCreateWindow failed\n");
		glfwTerminate();
		return 1;
	}
	glfwMakeContextCurrent(win);
	glfwSwapInterval(1); // vsync, matches Present(1,0) on Windows
	glfwSetWindowRefreshCallback(win,windowRefreshCb);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 4.0f;
	style.FrameRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.TabRounding = 3.0f;

	ImGui_ImplGlfw_InitForOpenGL(win,true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	App app;
	app.init();
	g_app = &app;

	while (!glfwWindowShouldClose(win)) {
		glfwPollEvents();
		if (glfwGetWindowAttrib(win,GLFW_ICONIFIED) != 0) {
			glfwWaitEventsTimeout(0.1); // don't spin while minimized
			continue;
		}
		renderFrame(win);
	}

	g_app = nullptr;

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
	return 0;
}
