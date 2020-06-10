/* -------------------------------------------------------------------------
 * main.c
 * Implements simulation of a single FMU instance
 * that implements the "FMI for Co-Simulation 1.0" interface.
 * Command syntax: see printHelp()
 * Simulates the given FMU from t = 0 .. tEnd with fixed step size h and
 * writes the computed solution to file 'result.csv'.
 * The CSV file (comma-separated values) may e.g. be plotted using 
 * OpenOffice Calc or Microsoft Excel. 
 * This progamm demonstrates basic use of an FMU.
 * Real applications may use advanced master algorithms to cosimulate 
 * many FMUs, limit the numerical error using error estimation
 * and back-stepping, provide graphical plotting utilities, debug support,
 * and user control of parameter and start values, or perform a clean
 * error handling (e.g. call freeSlaveInstance when a call to the fmu
 * returns with error). All this is missing here.
 *
 * Revision history
 *  22.08.2011 initial version released in FMU SDK 1.0.2
 *
 * Free libraries and tools used to implement this simulator:
 *  - header files from the FMU specification
 *  - eXpat 2.0.1 XML parser, see http://expat.sourceforge.net
 *  - 7z.exe 4.57 zip and unzip tool, see http://www.7-zip.org
 * Author: Jakob Mauss
 * Copyright QTronic GmbH. All rights reserved.
 * -------------------------------------------------------------------------*/

#include<winsock2.h> 
#define WIN32_LEAN_AND_MEAN
#include<windows.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fmi_cs.h"
#include "sim_support.h"
#include<assert.h>
#include<WS2tcpip.h>
#pragma comment(lib, "ws2_32")  
#pragma warning(disable:4996)

#define DB_BUFSIZE 8196

FMU fmu; // the fmu to simulate

//influxdb exit errorly
int pexit(const char * msg)
{
	perror(msg);
	exit(1);
	return 0;
}

//Model Open，加载fmu文件，会将其解压至临时目录，解析xml文件(到 fmu.modelDescription)，并加载dll文件，最后会将这些资源都free掉
void TwinOpen(TwinModel* twin) {
	//load fmu
	loadFMU(twin->fmuFileName);
	twin->fmu = fmu;

	//connect to influxdb
	WSADATA wsadata;
	int sockfd;
	SOCKADDR_IN serv_addr; /* static is zero filled on start up */
	int port = twin->port;
	const char* ip_address = twin->ip_address;

	WSAStartup(0x0202, &wsadata);
	if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0) 
		pexit("socket() failed");
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.S_un.S_addr = 0;
	serv_addr.sin_port = 0;
	bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(SOCKADDR_IN));
	InetPton(AF_INET, TEXT(ip_address), &serv_addr.sin_addr.S_un.S_addr);
	serv_addr.sin_port = ntohs(port);
	if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(SOCKADDR_IN)) != NOERROR)
		pexit("connect() failed");
	//后面写入数据时会用到
	twin->sockfd = sockfd;
}

//Model Close，释放FMU资源，释放dll，释放由xml解析得到的modelDescriprion，并且删除之前解压到临时目录的文件
void TwinClose(TwinModel* twin) {
	FMU* fmu = &(twin->fmu);
	int sockfd = twin->sockfd;

	// release FMU 
	#if WINDOWS
	FreeLibrary(fmu->dllHandle);
	#else /* WINDOWS */
	dlclose(fmu.dllHandle);
	#endif /* WINDOWS */
	freeElement(fmu->modelDescription);
	deleteUnzippedFiles();
	//close influxdb socket
	closesocket(sockfd);
	WSACleanup();
}

//创建资源，实例化fmu对象，并进行初始化，并且返回值类型为fmiComponent，后面的仿真函数将会用到该值
int TwinInitialize(TwinModel* twin){
	FMU *fmu = &(twin->fmu);
	double tEnd = twin->tEnd;
	fmiBoolean loggingOn = twin->loggingOn;

	ModelDescription* md;            // handle to the parsed XML file
	const char* guid;                // global unique id of the fmu
	fmiCallbackFunctions callbacks;  // called by the model during simulation
	char* fmuLocation = getTempFmuLocation(); // path to the fmu as URL, "file://C:\QTronic\sales"
	const char* mimeType = "application/x-fmu-sharedlibrary"; // denotes tool in case of tool coupling
	fmiReal timeout = 1000;          // wait period in milli seconds, 0 for unlimited wait period"
	fmiBoolean visible = fmiFalse;   // no simulator user interface
	fmiBoolean interactive = fmiFalse; // simulation run without user interaction
	fmiStatus fmiFlag;               // return code of the fmu functions
	double tStart = 0;               // start time
	fmiComponent c;                  // instance of the fmu 

	// instantiate the fmu
	md = fmu->modelDescription;
	guid = getString(md, att_guid);
	twin->guid = guid;
	callbacks.logger = fmuLogger;
	callbacks.allocateMemory = calloc;
	callbacks.freeMemory = free;
	callbacks.stepFinished = NULL; // fmiDoStep has to be carried out synchronously
	c = fmu->instantiateSlave(getModelIdentifier(md), guid, fmuLocation, mimeType,
						   timeout, visible, interactive, callbacks, loggingOn);
	free(fmuLocation);
	if (!c) return error("could not instantiate model");

	// StopTimeDefined=fmiFalse means: ignore value of tEnd
	fmiFlag = fmu->initializeSlave(c, tStart, fmiTrue, tEnd);
	if (fmiFlag > fmiWarning)  return error("could not initialize model");
	//后面仿真时会用到
	twin->c = c;
	return 1; // success
}

//一次性仿真完整个过程，所有变量写入csv
int TwinSimulation(TwinModel* twin) {
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	char separator = twin->separator;
	double tEnd = twin->tEnd;
	double h = twin->h;

	FILE* file;
	double tStart = 0;               // start time
	double time;
	double hh = h;
	int nSteps = 0;
	fmiStatus fmiFlag;               // return code of the fmu functions
	// open result file
	if (!(file = fopen(RESULT_FILE, "w"))) {
		printf("could not write %s because:\n", RESULT_FILE);
		printf("    %s\n", strerror(errno));
		return 0; // failure
	}

	//写csv
	// output solution for time t0
	outputRow(fmu, c, tStart, file, separator, fmiTrue);  // output column names
	outputRow(fmu, c, tStart, file, separator, fmiFalse); // output values

	// enter the simulation loop
	time = tStart;
	while (time < tEnd) {
		// check not to pass over end time
		if (h > tEnd - time) {
			hh = tEnd - time;
		}
		fmiFlag = fmu->doStep(c, time, hh, fmiTrue);
		if (fmiFlag != fmiOK)  return error("could not complete simulation of the model");
		time += hh;
		//写csv
		outputRow(fmu, c, time, file, separator, fmiFalse); // output values for this step
		nSteps++;
	}

	// end simulation
	fmiFlag = fmu->terminateSlave(c);
	fmu->freeSlaveInstance(c);
	fclose(file);

	// print simulation summary 
	printf("Simulation from %g to %g terminated successful\n", tStart, tEnd);
	printf("  steps ............ %d\n", nSteps);
	printf("  fixed step size .. %g\n", h);
	return 1; // success
}

//重置模型
void TwinReset(TwinModel* twin) {
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	fmu->resetSlave(c);
}

//设置初值，valueRef和value是两个数组
void TwinSetInputs(TwinModel* twin) {
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	int setNumber = twin->setNumber;
	int *valueRef = twin->set_valueRef;
	double *value = twin->set_value;
	/**
		test setReal through API
		**/
	if (setNumber > 0) {
		for (int i = 0; i < setNumber; i++) {
			const fmiValueReference vr1[1] = { valueRef[i] };
			fmiReal    value1[1] = { value[i] };
			fmu->setReal(c, vr1, 1, value1);
		}
	}
}

//按需获取模型仿真输出（仿真结束时这些变量的值）
void TwinGetOutputs(TwinModel* twin){
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	int getNumber = twin->getNumber;
	int *valueSeq = twin->get_valueSeq;
	double *output = twin->output;

	ScalarVariable** vars = fmu->modelDescription->modelVariables;
	for (int i = 0; i < getNumber; i++) {
		ScalarVariable* sv = vars[valueSeq[i]];
		fmiValueReference vr = getValueReference(sv);
		const char* name = getName(sv);
		fmiReal value;
		fmu->getReal(c, &vr, 1, &value);
		output[i] = value;
		printf("------------------------------------ %s is %.16g\n", name, output[i]);
	}
}

//传入参数指定了modelDescription中的某一个ScalarVariable，返回其名称
char *TwinGetVariableName(ScalarVariable* sv) {
	//obtain name of  the model variable
	char* name = getName(sv);
	return name;
}

//返回其valueReference
int TwinGetVariableReference(ScalarVariable* sv) {
	fmiValueReference vr = getValueReference(sv);
	return vr;
}

//返回其type
int TwinGetVariableType(ScalarVariable* sv) {
	switch (sv->typeSpec->type) {
	case elm_Real:
		return 1;
		break;
	case elm_Integer:
		return 2;
		break;
	case elm_Enumeration:
		return 3;
		break;
	case elm_Boolean:
		return 4;
		break;
	case elm_String:
		return 5;
	default: return 0;
	}
}

//返回其初值
double TwinGetVariableInit(ScalarVariable* sv, TwinModel* twin) {
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	fmiReal r;
	fmiValueReference vr = getValueReference(sv);
	fmu->getReal(c, &vr, 1, &r);
	return r;
}

//逐步仿真，写influxdb
double TwinSimulationByStep(TwinModel* twin, double time, char *header, char *body) {
	const char* fmuFileName = twin->fmuFileName;
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	char separator = twin->separator;
	double tEnd = twin->tEnd;
	double h = twin->h;	//设定步长
	int sockfd = twin->sockfd;//用于写influxdb
	int getNumber = twin->getNumber;
	int *valueSeq = twin->get_valueSeq;
	double *output = twin->output;
	const char* guid = twin->guid;

	double hh = h;   //实际仿真步长
	fmiStatus fmiFlag;               // return code of the fmu functions

	// check not to pass over end time
	if (h > tEnd - time) {
		hh = tEnd - time;
	}
	fmiFlag = fmu->doStep(c, time, hh, fmiTrue);
	//if (fmiFlag != fmiOK)  return error("could not complete simulation of the model");
	time += hh;
	//写influxdb
	int ret;
	sprintf(body, "%s,global_id=%s timestamp=%.16g,", fmuFileName, guid, time);
	ScalarVariable** vars = fmu->modelDescription->modelVariables;
	for (int i = 0; i < getNumber; i++) {
		ScalarVariable* sv = vars[valueSeq[i]];
		fmiValueReference vr = getValueReference(sv);
		const char* name = getName(sv);
		fmiReal value;
		fmu->getReal(c, &vr, 1, &value);
		strcat(body, name);
		strcat(body, "=");
		char value_temp[20];
		sprintf(value_temp, "%.16g", value);
		strcat(body, value_temp);
		if (i == getNumber - 1) {
			strcat(body, "\n");
		}
		else {
			strcat(body, ",");
		}
	}
	sprintf(header,
		"POST /write?db=%s&u=%s&p=%s HTTP/1.1\r\nHost: influx:8086\r\nContent-Length: %ld\r\n\r\n",
		twin->database, twin->username, twin->password, strlen(body));
	ret = send(sockfd, header, strlen(header), 0);
	if (ret < 0)
		pexit("Write Header request to InfluxDB failed");
	ret = send(sockfd, body, strlen(body), 0);
	if (ret < 0)
		pexit("Write Data Body to InfluxDB failed");
	return time; // success
}



int main(int argc, char *argv[]) {
	TwinModel twin;
    parseArguments(argc, argv, &twin);	

	twin.port = 8086;
	twin.ip_address = "127.0.0.1";
	twin.database = "rt_test";
	twin.username = "rw_db";
	twin.password = "dansan";

    TwinOpen(&twin);
    // run the simulation
    //printf("FMU Simulator: run '%s' from t=0..%g with step size h=%g, loggingOn=%d, csv separator='%c'\n",
            //fmuFileName, tEnd, h, loggingOn, csv_separator);
	TwinInitialize(&twin);
	TwinSetInputs(&twin);

	////output model properties
	//ScalarVariable** vars = twin.fmu.modelDescription->modelVariables;
	//for (int k = 0; vars[k]; k++) {
	//	ScalarVariable* sv = vars[k];
	//	printf("name %d is %s\n", k, TwinGetVariableName(sv));
	//	printf("valueReference %d is %d\n", k, TwinGetVariableReference(sv));
	//	//printf("valueDescription %d is %s\n", k, TwinGetVariableDescription(sv));
	//	printf("valueType %d is %d\n", k, TwinGetVariableType(sv));
	//	printf("valueInit %d is %.16g\n", k, TwinGetVariableInit(sv, &twin));
	//}

	//simulate step by step
	
	////csv相关
	//FILE* file;
	//// open result file
	//if (!(file = fopen(RESULT_FILE, "w"))) {
	//	printf("could not write %s because:\n", RESULT_FILE);
	//	printf("    %s\n", strerror(errno));
	//	return 0; // failure
	//}
	double time = 0;
	char header[DB_BUFSIZE];
	char body[DB_BUFSIZE];
	//// output solution for time t0
	//outputRow(&(twin.fmu), twin.c, time, file, twin.separator, fmiTrue);  // output column names
	//outputRow(&(twin.fmu), twin.c, time, file, twin.separator, fmiFalse); // output values

	while (time < twin.tEnd) {
		time = TwinSimulationByStep(&twin, time, header, body);
		TwinGetOutputs(&twin);
		/*printf("------------------------------------time is %lf\n", time);
		for (int i = 0; i < twin.getNumber; i++) {
			printf("------------------------------------output %d is %.16g\n", i, twin.output[i]);
		}
		printf("------------------------------------\n");*/
	}
	// end simulation
	(&(twin.fmu))->terminateSlave(twin.c);
	(&(twin.fmu))->freeSlaveInstance(twin.c);
	//fclose(file);


	/*TwinSimulation(&twin);
	TwinGetOutputs(&twin);
	for (int i = 0; i < twin.getNumber; i++) {
		printf("------------------------------------output %d is %.16g\n", i, twin.output[i]);
	}*/
    printf("CSV file '%s' written\n", RESULT_FILE);
	//TwinReset(&fmu, c);
	//c = TwinInitialize(&fmu, tEnd, loggingOn);
	//TwinSimulation(&fmu, c, csv_separator, tEnd, h);
	//printf("CSV file '%s' written\n", RESULT_FILE);
	TwinClose(&twin);
    return EXIT_SUCCESS;
}

