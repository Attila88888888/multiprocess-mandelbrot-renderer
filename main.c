// gcc -std=gnu11 main.c -o fractal_renderer.exe

#include <stdio.h>	 //for file I/O
#include <stdint.h>	 //for fixed-width integer types
#include <stdlib.h>	 //for atoi
#include <math.h>	 //for sqrt
#include <complex.h> //for complex numbers
#include <windows.h> //for time measurement, shared memory
#include <string.h>	 //for strcmp

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// CONSTANTS
#define WIDTH 1024
#define HEIGHT 1024
#define COLOR_AMOUNT 3
#define MAX_ITER 1000
#define SHM_NAME "Local\\fractal_shm"

HANDLE create_shared_buffer(size_t buffer_size, uint8_t **out_buffer)
{
	HANDLE hMapping = CreateFileMappingA(
		INVALID_HANDLE_VALUE,			  // not backed by a real file → must specify size
		NULL,							  // default security
		PAGE_READWRITE,					  // read+write access
		0,								  // high 32 bits of size (we don't need it)
		buffer_size,					  // low 32 bits of size
		SHM_NAME						  // name shared with child processes so they can open it
	);

	if (hMapping == NULL)
	{
		fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError());
		return NULL;
	}

	*out_buffer = (uint8_t *)MapViewOfFile(
		hMapping, FILE_MAP_ALL_ACCESS, 0, 0, buffer_size);

	if (*out_buffer == NULL)
	{
		fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
		CloseHandle(hMapping);
		return NULL;
	}

	return hMapping;
}

HANDLE attach_shared_buffer(size_t buffer_size, uint8_t **out_buffer)
{
	HANDLE hMapping = OpenFileMappingA( //open existing mapping created by parent
		FILE_MAP_ALL_ACCESS, 			//access mode
		FALSE,	 						//cannot be inherited
		SHM_NAME 						//same name parent uses
	);

	if (hMapping == NULL)
	{
		fprintf(stderr, "OpenFileMapping failed: %lu\n", GetLastError());
		return NULL;
	}

	*out_buffer = (uint8_t *)MapViewOfFile(
		hMapping, 				//the mapping handle
		FILE_MAP_ALL_ACCESS, 	//access mode
		0, 
		0,
		buffer_size
	);

	if (*out_buffer == NULL)
	{
		fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
		CloseHandle(hMapping);
		return NULL;
	}

	return hMapping;
}

void release_shared_buffer(uint8_t *buffer, HANDLE hMapping)
{
	UnmapViewOfFile(buffer);
	CloseHandle(hMapping);
}

int spawn_process(int process_id, int total_processes, HANDLE *out_handle)
{
	STARTUPINFOA startup_info = {0};
	startup_info.cb = sizeof(startup_info);
	PROCESS_INFORMATION process_info = {0};

	char cmdline[256];
	snprintf(cmdline, sizeof(cmdline), "fractal_renderer.exe --process %d %d", process_id, total_processes); //cmd line for child processes

	BOOL create_succeeded = CreateProcessA(
		NULL, 			//null = use first token in cmd line
		cmdline,
		NULL, 			//default security
		NULL, 			//default security
		FALSE, 			//cannot be inherited
		0, 				//no creation flags
		NULL,			//parent's environment 
		NULL,			//parent's current directory
		&startup_info,
		&process_info
	);
	if (!create_succeeded)
	{
		fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
		return 0;
	}

	CloseHandle(process_info.hThread);
	*out_handle = process_info.hProcess;

	return 1;
}

int write_png_file(const char *filename, uint8_t *buffer)
{
	int bytes_per_row = WIDTH * COLOR_AMOUNT;
    int rendered_file = stbi_write_png(filename, WIDTH, HEIGHT, COLOR_AMOUNT, buffer, bytes_per_row);
    if (!rendered_file)
    {
        fprintf(stderr, "stbi_write_png failed\n");
        return 0;
    }
    return 1;
}

void compute_pixel_color(int iter, uint8_t *r, uint8_t *g, uint8_t *b)
{
	if (iter == MAX_ITER)
	{
		*r = 0;
		*g = 0;
		*b = 0;
		return;
	}
	double escape_t = sqrt((double)iter / MAX_ITER);
	*r = (uint8_t)(9  * (1 - escape_t) * escape_t       * escape_t       * escape_t * 255);
	*g = (uint8_t)(15 * (1 - escape_t) * (1 - escape_t) * escape_t       * escape_t * 255);
	*b = (uint8_t)(9  * (1 - escape_t) * (1 - escape_t) * (1 - escape_t) * escape_t * 255);
}

void render_rows(uint8_t *buffer, int process_id, int total_processes)
{
	double x_min = -2.0; 
	double x_max = 1.0;
	double y_min = -1.5;
	double y_max = 1.5;

	double x_range = x_max - x_min;
	double y_range = y_max - y_min;

	for (int y = process_id; y < HEIGHT; y += total_processes)
	{
		for (int x = 0; x < WIDTH; x += 1)
		{
			double cx = x_min + (x / (double)WIDTH) * x_range;
			double cy = y_min + (y / (double)HEIGHT) * y_range;

			double complex c = cx + cy * I;
			double complex z = 0.0 + 0.0 * I;

			int iter;
			for (iter = 0; iter < MAX_ITER; iter += 1)
			{
				if (creal(z) * creal(z) + cimag(z) * cimag(z) >= 4.0) break; //|z|^2 >= 4 means it will escape to infinity
				z = z * z + c;
			}

			uint8_t r, g, b;
			compute_pixel_color(iter, &r, &g, &b);

			size_t byte_offset = (y * WIDTH + x) * COLOR_AMOUNT;
			buffer[byte_offset + 0] = r;
			buffer[byte_offset + 1] = g;
			buffer[byte_offset + 2] = b;
		}
	}
}

int parent_main(int argc, char **argv)
{
	int number_of_processes = 1;
	if (argc >= 2)
	{
		number_of_processes = atoi(argv[1]); // convert argument to int
		if (number_of_processes < 1)
			number_of_processes = 1;
		if (number_of_processes > MAXIMUM_WAIT_OBJECTS)
		{
			//64 on Windows; we won't hit this for the assignment's number_of_processes values
			fprintf(stderr, "Number of processes is too large (max %d)\n", MAXIMUM_WAIT_OBJECTS);
			return 1;
		}
	}

	const int32_t buffer_size = WIDTH * HEIGHT * COLOR_AMOUNT;

	uint8_t *pixel_buffer;
	HANDLE hMapping = create_shared_buffer(buffer_size, &pixel_buffer);
	if (hMapping == NULL)
		return 1;

	LARGE_INTEGER timer_frequency, render_start_time, render_end_time; //64-bit integer type
	QueryPerformanceFrequency(&timer_frequency); //get high-resolution timer frequency

	QueryPerformanceCounter(&render_start_time); //start timer

	HANDLE wait_handles[MAXIMUM_WAIT_OBJECTS];

	for (int i = 0; i < number_of_processes; i += 1)
	{
		if (!spawn_process(i, number_of_processes, &wait_handles[i]))
		{
			release_shared_buffer(pixel_buffer, hMapping);
			return 1;
		}
	}

	WaitForMultipleObjects(
		number_of_processes,
		wait_handles,		//handles to wait for
		TRUE,				//wait for all processes to finish
		INFINITE			//wait indefinitely
	);

	for (int i = 0; i < number_of_processes; i += 1)
	{
		CloseHandle(wait_handles[i]);
	}

	QueryPerformanceCounter(&render_end_time); // end timer
	double elapsed_time = (double)(render_end_time.QuadPart - render_start_time.QuadPart) / timer_frequency.QuadPart;
	printf("Time taken: %f seconds\n", elapsed_time);

	if (!write_png_file("fractal_renderer.png", pixel_buffer))
	{
		release_shared_buffer(pixel_buffer, hMapping);
		return 1;
	}

	release_shared_buffer(pixel_buffer, hMapping);

	return 0;
}

int process_main(int argc, char **argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "process usage: --process <process_id> <total_processes>\n");
		return 1;
	}
	int process_id = atoi(argv[2]);
	int total_processes = atoi(argv[3]);
	const int32_t buffer_size = WIDTH * HEIGHT * COLOR_AMOUNT;

	uint8_t *pixel_buffer;
	HANDLE hMapping = attach_shared_buffer(buffer_size, &pixel_buffer);
	if (hMapping == NULL)
	{
		return 1;
	}

	render_rows(pixel_buffer, process_id, total_processes);

	release_shared_buffer(pixel_buffer, hMapping);

	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--process") == 0)
	{
		return process_main(argc, argv);
	}

	return parent_main(argc, argv);
}