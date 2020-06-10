/* -------------------------------------------------------------------------
 * main.c
 * Implements simulation of a single FMU instance
 * that implements the "FMI for Co-Simulation 1.0" interface.
 * Command syntax: see printHelp()
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
#include<signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fmi_cs.h"
#include "sim_support.h"
#include<assert.h>
#include<WS2tcpip.h>
#include "zlog.h"
#include <math.h>
#pragma comment(lib, "ws2_32")  
#pragma warning(disable:4996)
#define DB_BUFSIZE 8196

FMU fmu; // the fmu to simulate
//zlog
int rc;
zlog_category_t *zc;

//Open model. Connect to InfluxDB
void TwinOpen(TwinModel* twin) {
	zlog_info(zc, "start loading '%s'\r\n",twin->fmuFileName);
	//load fmu
	loadFMU(twin->fmuFileName);
	twin->fmu = fmu;
	zlog_info(zc, "load '%s' successfully\r\n", twin->fmuFileName);
	//connect to influxdb
	zlog_info(zc, "start connecting to InfluxDB %s : %d\r\n",twin->ip_address,twin->port);
	WSADATA wsadata;
	int sockfd;
	SOCKADDR_IN serv_addr; 
	int port = twin->port;
	const char* ip_address = twin->ip_address;
	WSAStartup(0x0202, &wsadata);
	if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		zlog_error(zc, "InfluxDB socket() failed\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.S_un.S_addr = 0;
	serv_addr.sin_port = 0;
	bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(SOCKADDR_IN));
	InetPton(AF_INET, TEXT(ip_address), &serv_addr.sin_addr.S_un.S_addr);
	serv_addr.sin_port = ntohs(port);
	if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(SOCKADDR_IN)) != NOERROR) {
		zlog_error(zc, "InfluxDB connect() failed\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	//后面写入数据时会用到
	twin->sockfd = sockfd;
	zlog_info(zc, "connect to InfluxDB %s : %d successfully\r\n", twin->ip_address, twin->port);
}

//Close model. Disconnect from InfluxDB
void TwinClose(TwinModel* twin) {
	FMU* fmu = &(twin->fmu);
	int sockfd = twin->sockfd;
	//end simulation
	fmu->terminateSlave(twin->c);
	fmu->freeSlaveInstance(twin->c);
	zlog_info(zc, "end simulation successfully\r\n");
	// release FMU 
	#if WINDOWS
	FreeLibrary(fmu->dllHandle);
	#else /* WINDOWS */
	dlclose(fmu.dllHandle);
	#endif /* WINDOWS */
	freeElement(fmu->modelDescription);
	deleteUnzippedFiles();
	zlog_info(zc, "release '%s' successfully\r\n",twin->fmuFileName);
	//close influxdb socket
	closesocket(sockfd);
	WSACleanup();
	zlog_info(zc, "disconnect from InfluxDB  %s : %d successfully\r\n", twin->ip_address, twin->port);
}

//Instantiate and initialize fmu
void TwinInitialize(TwinModel* twin){
	FMU *fmu = &(twin->fmu);
	double tEnd = twin->tEnd;
	ModelDescription* md;            // handle to the parsed XML file
	const char *fmuid;             // global unique id of the fmu
	int guid = 0;                // global unique id of the simulation
	fmiCallbackFunctions callbacks;  // called by the model during simulation
	char* fmuLocation = getTempFmuLocation(); // path to the fmu as URL, "file://C:\QTronic\sales"
	const char* mimeType = "application/x-fmu-sharedlibrary"; // denotes tool in case of tool coupling
	fmiReal timeout = 1000;          // wait period in milli seconds, 0 for unlimited wait period"
	fmiBoolean visible = fmiFalse;   // no simulator user interface
	fmiBoolean interactive = fmiFalse; // simulation run without user interaction
	fmiBoolean loggingOn = fmiFalse; //open or close log
	fmiStatus fmiFlag;               // return code of the fmu functions
	double tStart = 0;               // start time
	fmiComponent c;                  // instance of the fmu 

	// instantiate the fmu
	zlog_info(zc, "start instantiating fmu\r\n");
	md = fmu->modelDescription;
	fmuid = getString(md, att_guid);
	//obtain simulation id
	FILE *fp;
	errno_t err;
	err = fopen_s(&fp, "guid.txt", "r");
	if (err != 0) {
		zlog_error(zc, "could not open guid.txt\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	fscanf(fp, "%d", &guid);
	fclose(fp);
	guid = guid + 1;
	err = fopen_s(&fp, "guid.txt", "w");
	if (err != 0) {
		zlog_error(zc, "could not open guid.txt\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	fprintf_s(fp, "%d", guid);

	fclose(fp);
	twin->guid = guid;
	callbacks.logger = fmuLogger;
	callbacks.allocateMemory = calloc;
	callbacks.freeMemory = free;
	callbacks.stepFinished = NULL; // fmiDoStep has to be carried out synchronously
	c = fmu->instantiateSlave(getModelIdentifier(md), fmuid, fmuLocation, mimeType,
						   timeout, visible, interactive, callbacks, loggingOn);
	free(fmuLocation);
	if (!c) {
		zlog_error(zc, "could not instantiate model\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	zlog_info(zc, "instantiate fmu successfully\r\n");

	zlog_info(zc, "start initializing fmu\r\n");
	fmiFlag = fmu->initializeSlave(c, tStart, fmiTrue, tEnd);
	if (fmiFlag > fmiWarning) {
		zlog_error(zc, "could not initialize model\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	zlog_info(zc, "initialize fmu successfully\r\n");
	//后面仿真时会用到
	twin->c = c;
}

//一次性仿真完整个过程
void TwinSimulation(TwinModel* twin, char *header, char *body, char *result) {
	const char* fmuFileName = twin->fmuFileName;
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	double tEnd = twin->tEnd;
	double h = twin->h;
	int sockfd = twin->sockfd;//用于写influxdb
	int getNumber = twin->getNumber;
	int *get_valueSeq = twin->get_valueSeq;
	int setNumber = twin->setNumber;
	int *set_valueSeq = twin->set_valueSeq;
	int guid = twin->guid;
	double tStart = 0;               // start time
	double time;
	double hh = h;
	fmiStatus fmiFlag;               // return code of the fmu functions
	zlog_info(zc, "start simulating the whole process and writing data to InfluxDB\r\n");
	// enter the simulation loop
	time = tStart;
	//influxdb
	int ret;
	//InfluxDB表名,name
	char temp[1000];
	strcpy(temp, fmuFileName);
	char *name, *token;
	name = NULL;
	token = strtok(temp, "/");
	while (token != NULL) {
		name = token;
		token = strtok(NULL, "/");
	}
	//0时刻的值写入
	sprintf(body, "%s,global_id=%d timestamp=%g,", name, guid, time);
	ScalarVariable** vars = fmu->modelDescription->modelVariables;
	for (int i = 0; i < setNumber; i++) {
		ScalarVariable* sv = vars[set_valueSeq[i]];
		fmiValueReference vr = getValueReference(sv);
		const char* name = getName(sv);
		char finalName[1000];
		int k = 0;
		while (*name) {
			if (*name != ' ') {
				if (*name == ',') {
					finalName[k] = '.';
				}
				else {
					finalName[k] = *name;
				}
				k++;
			}
			name++;
		}
		finalName[k] = 0;
		strcat(body, finalName);
		strcat(body, "=");
		char value_temp[20];
		fmiReal value_r;
		fmiInteger value_i;
		switch (sv->typeSpec->type) {
		case elm_Real:
			fmu->getReal(c, &vr, 1, &value_r);
			sprintf(value_temp, "%.16g", value_r);
			strcat(body, value_temp);
			break;
		case elm_Integer:
			fmu->getInteger(c, &vr, 1, &value_i);
			sprintf(value_temp, "%d", value_i);
			strcat(body, value_temp);
			break;
		}
		if (getNumber == 0) {
			if (i == setNumber - 1) {
				strcat(body, "\n");
			}
			else {
				strcat(body, ",");
			}
		}
		else {
			strcat(body, ",");
		}
	}
	for (int i = 0; i < getNumber; i++) {
		ScalarVariable* sv = vars[get_valueSeq[i]];
		fmiValueReference vr = getValueReference(sv);
		const char* name = getName(sv);
		char finalName[1000];
		int k = 0;
		while (*name) {
			if (*name != ' ') {
				if (*name == ',') {
					finalName[k] = '.';
				}
				else {
					finalName[k] = *name;
				}
				k++;
			}
			name++;
		}
		finalName[k] = 0;
		strcat(body, finalName);
		strcat(body, "=");
		char value_temp[20];
		fmiReal value_r;
		fmiInteger value_i;
		switch (sv->typeSpec->type) {
		case elm_Real:
			fmu->getReal(c, &vr, 1, &value_r);
			sprintf(value_temp, "%.16g", value_r);
			strcat(body, value_temp);
			break;
		case elm_Integer:
			fmu->getInteger(c, &vr, 1, &value_i);
			sprintf(value_temp, "%d", value_i);
			strcat(body, value_temp);
			break;
		}
		if (i == getNumber - 1) {
			strcat(body, "\n");
		}
		else {
			strcat(body, ",");
		}
	}
	sprintf(header,
		"POST /write?db=%s&u=%s&p=%s HTTP/1.1\r\nHost: influx:8086\r\nContent-Length: %zd\r\n\r\n",
		twin->database, twin->username, twin->password, strlen(body));
	ret = send(sockfd, header, strlen(header), 0);
	if (ret < 0) {
		zlog_error(zc, "write header request to InfluxDB failed\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	ret = send(sockfd, body, strlen(body), 0);
	if (ret < 0) {
		zlog_error(zc, "write data body to InfluxDB failed\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	//zlog_info(zc, "write initial data to InfluxDB successfully\r\n");
	ret = recv(sockfd, result,DB_BUFSIZE, 0);
	if (ret < 0) {
		zlog_error(zc, "read the result from InfluxDB failed\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}
	result[ret] = 0; /* terminate string */
	char *buf[2], *p;
	p = NULL;
	p = strtok(result, " ");      // 分割同一字符串，第一次调用时传入字符串的首地址
	buf[0] = p;
	p = strtok(NULL, " ");      // 再次调用分割时指针要变为NULL, 也就是这里的第一个参数,分割的字符串还是str
	buf[1] = p;
	if (strcmp(buf[1], "204") == 0) {
		zlog_info(zc, "write initial data to InfluxDB successfully, status code is %s\r\n", buf[1]);
	}
	else {
		zlog_error(zc, "write initial data to InfluxDB failed, status code is %s\r\n", buf[1]);
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
	}

	while (time < tEnd) {
		zlog_info(zc, "FMU simulate a step from t=%g\r\n", time);
		// check not to pass over end time
		if (h > tEnd - time) {
			hh = tEnd - time;
		}
		fmiFlag = fmu->doStep(c, time, hh, fmiTrue);
		if (fmiFlag != fmiOK) {
			zlog_error(zc, "could not complete simulation of the model\r\n");
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
		}
		zlog_info(zc, "FMU simulate the step from t=%g successfully\r\n", time);
		time += hh;
		//write to InfluxDB
		sprintf(body, "%s,global_id=%d timestamp=%g,", name, guid, time);
		ScalarVariable** vars = fmu->modelDescription->modelVariables;
		for (int i = 0; i < setNumber; i++) {
			ScalarVariable* sv = vars[set_valueSeq[i]];
			fmiValueReference vr = getValueReference(sv);
			const char* name = getName(sv);
			char finalName[1000];
			int k = 0;
			while (*name) {
				if (*name != ' ') {
					if (*name == ',') {
						finalName[k] = '.';
					}
					else {
						finalName[k] = *name;
					}
					k++;
				}
				name++;
			}
			finalName[k] = 0;
			strcat(body, finalName);
			strcat(body, "=");
			char value_temp[20];
			fmiReal value_r;
			fmiInteger value_i;
			switch (sv->typeSpec->type) {
			case elm_Real:
				fmu->getReal(c, &vr, 1, &value_r);
				sprintf(value_temp, "%.16g", value_r);
				strcat(body, value_temp);
				break;
			case elm_Integer:
				fmu->getInteger(c, &vr, 1, &value_i);
				sprintf(value_temp, "%d", value_i);
				strcat(body, value_temp);
				break;
			}
			if (getNumber == 0) {
				if (i == setNumber - 1) {
					strcat(body, "\n");
				}
				else {
					strcat(body, ",");
				}
			}
			else {
				strcat(body, ",");
			}
		}
		for (int i = 0; i < getNumber; i++) {
			ScalarVariable* sv = vars[get_valueSeq[i]];
			fmiValueReference vr = getValueReference(sv);
			const char* name = getName(sv);
			char finalName[1000];
			int k = 0;
			while (*name) {
				if (*name != ' ') {
					if (*name == ',') {
						finalName[k] = '.';
					}
					else {
						finalName[k] = *name;
					}
					k++;
				}
				name++;
			}
			finalName[k] = 0;
			strcat(body, finalName);
			strcat(body, "=");
			char value_temp[20];
			fmiReal value_r;
			fmiInteger value_i;
			switch (sv->typeSpec->type) {
			case elm_Real:
				fmu->getReal(c, &vr, 1, &value_r);
				sprintf(value_temp, "%.16g", value_r);
				strcat(body, value_temp);
				break;
			case elm_Integer:
				fmu->getInteger(c, &vr, 1, &value_i);
				sprintf(value_temp, "%d", value_i);
				strcat(body, value_temp);
				break;
			}
			if (i == getNumber - 1) {
				strcat(body, "\n");
			}
			else {
				strcat(body, ",");
			}
		}
		sprintf(header,
			"POST /write?db=%s&u=%s&p=%s HTTP/1.1\r\nHost: influx:8086\r\nContent-Length: %zd\r\n\r\n",
			twin->database, twin->username, twin->password, strlen(body));
		ret = send(sockfd, header, strlen(header), 0);
		if (ret < 0) {
			zlog_error(zc, "write header request to InfluxDB failed\r\n");
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
		}
		ret = send(sockfd, body, strlen(body), 0);
		if (ret < 0) {
			zlog_error(zc, "write data body to InfluxDB failed\r\n");
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
		}
		//zlog_info(zc, "write data to InfluxDB successfully\r\n");
		ret = recv(sockfd, result,DB_BUFSIZE, 0);
		if (ret < 0) {
			zlog_error(zc, "read the result from InfluxDB failed\r\n");
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
		}
		result[ret] = 0; /* terminate string */
		char *buf[2], *p;
		p = NULL;
		p = strtok(result, " ");      // 分割同一字符串，第一次调用时传入字符串的首地址
		buf[0] = p;
		p = strtok(NULL, " ");      // 再次调用分割时指针要变为NULL, 也就是这里的第一个参数,分割的字符串还是str
		buf[1] = p;
		if (strcmp(buf[1], "204") == 0) {
			zlog_info(zc, "write data to InfluxDB successfully, status code is %s\r\n", buf[1]);
		}
		else {
			zlog_error(zc, "write data to InfluxDB failed, status code is %s\r\n", buf[1]);
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
		}
	}
	zlog_info(zc, "simulate the whole process and write data to InfluxDB successfully\r\n");
}

//重置模型
void TwinReset(TwinModel* twin) {
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	zlog_info(zc, "start resetting fmu\r\n");
	fmu->resetSlave(c);
	zlog_info(zc, "reset fmu successfully\r\n");
}

//设置初值
void TwinSetInputs(TwinModel* twin) {
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	int setNumber = twin->setNumber;
	int *valueSeq = twin->set_valueSeq;
	double *value = twin->set_value;
	//int *valueRef = (int *)malloc((setNumber) * sizeof(int));
	//ScalarVariable** vars = fmu->modelDescription->modelVariables;
	//for (int i = 0; i < setNumber; i++) {
	//	ScalarVariable* sv = vars[valueSeq[i]];
	//	fmiValueReference vr = getValueReference(sv);
	//	valueRef[i] = vr;
	//}
	///**
	//	setReal through API
	//	**/
	//zlog_info(zc, "start setting inputs\r\n");
	//if (setNumber > 0) {
	//	for (int i = 0; i < setNumber; i++) {
	//		const fmiValueReference vr1[1] = { valueRef[i] };
	//		fmiReal value1[1] = { value[i] };
	//		fmu->setReal(c, vr1, 1, value1);
	//	}
	//}
	//zlog_info(zc, "set inputs successfully\r\n");
	zlog_info(zc, "start setting inputs\r\n");
	ScalarVariable** vars = fmu->modelDescription->modelVariables;
	for (int i = 0; i < setNumber; i++) {
		ScalarVariable* sv = vars[valueSeq[i]];
		fmiValueReference vr = getValueReference(sv);
		const fmiValueReference vr1[1] = { vr };
		switch (sv->typeSpec->type) {
			case elm_Real:
				fmiReal value1[1] = { value[i] };
				fmu->setReal(c, vr1, 1, value1);
				break;
			case elm_Integer:
				fmiInteger value2[1] = { (int)(value[i]) };
				fmu->setInteger(c, vr1, 1, value2);
				break;
		}
	}
	zlog_info(zc, "set inputs successfully\r\n");
}

//按需获取模型某些变量的值
void TwinGetOutputs(TwinModel* twin){
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	int getNumber = twin->getNumber;
	int *valueSeq = twin->get_valueSeq;
	double *output = twin->output;
	zlog_info(zc, "start obtaining value of specified variables\r\n");
	ScalarVariable** vars = fmu->modelDescription->modelVariables;
	for (int i = 0; i < getNumber; i++) {
		ScalarVariable* sv = vars[valueSeq[i]];
		fmiValueReference vr = getValueReference(sv);
		//const char* name = getName(sv);
		switch (sv->typeSpec->type) {
			case elm_Real:
				fmiReal value1;
				fmu->getReal(c, &vr, 1, &value1);
				output[i] = value1;
				//printf("-------------------%s is %.16g-------------------------\n", name, output[i]);
				break;
			case elm_Integer:
				fmiInteger value2;
				fmu->getInteger(c, &vr, 1, &value2);
				output[i] = value2;
				//printf("-------------------%s is %lf-------------------------\n", name, output[i]);
				break;
		}
	}
	zlog_info(zc, "obtain value of specified variables successfully\r\n");
}

//传入参数指定了modelDescription中的某一个ScalarVariable，返回其名称
const char *TwinGetVariableName(ScalarVariable* sv) {
	//obtain name of  the model variable
	zlog_info(zc, "start obtaining variable name\r\n");
	const char* name = getName(sv);
	zlog_info(zc, "obtain variable name successfully\r\n");
	return name;
}

//返回其causality
int TwinGetVariableCausality(ScalarVariable* sv) {
	zlog_info(zc, "start obtaining variable causality property\r\n");
	Enu causality = getCausality(sv);
	zlog_info(zc, "obtain variable causality property successfully\r\n");
	return causality;
}

//返回其valueReference
int TwinGetVariableReference(ScalarVariable* sv) {
	zlog_info(zc, "start obtaining variable valueReference\r\n");
	fmiValueReference vr = getValueReference(sv);
	zlog_info(zc, "obtain variable valueReference successfully\r\n");
	return vr;
}

//返回其type
int TwinGetVariableType(ScalarVariable* sv) {
	zlog_info(zc, "start obtaining variable type\r\n");
	switch (sv->typeSpec->type) {
		zlog_info(zc, "obtain variable type successfully\r\n");
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
	zlog_info(zc, "start obtaining variable init value\r\n");
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	fmiReal r;
	fmiValueReference vr = getValueReference(sv);
	fmu->getReal(c, &vr, 1, &r);
	zlog_info(zc, "obtain variable init value successfully\r\n");
	return r;
}

//逐步仿真，写influxdb
double TwinSimulationByStep(TwinModel* twin, double time, char *header, char *body, char *result) {
	const char* fmuFileName = twin->fmuFileName;
	FMU* fmu = &(twin->fmu);
	fmiComponent c = twin->c;
	double tEnd = twin->tEnd;
	double h = twin->h;	//设定步长
	int sockfd = twin->sockfd;//用于写influxdb
	int getNumber = twin->getNumber;
	int *get_valueSeq = twin->get_valueSeq;
	int setNumber = twin->setNumber;
	int *set_valueSeq = twin->set_valueSeq;
	int guid = twin->guid;
	double hh = h;   //实际仿真步长
	fmiStatus fmiFlag;               // return code of the fmu functions
	int ret;
	// check not to pass over end time
	if (h > tEnd - time) {
		hh = tEnd - time;
	}
	//InfluxDB表名,name
	char temp[1000];
	strcpy(temp, fmuFileName);
	char *name, *token;
	name = NULL;
	token = strtok(temp, "/");
	while (token != NULL) {
		name = token;
		token = strtok(NULL, "/");
	}
	//写0时刻的值
	if (fabs(time - 0) < 1e-15) {
		sprintf(body, "%s,global_id=%d timestamp=%g,", name, guid, time);
		ScalarVariable** vars = fmu->modelDescription->modelVariables;
		for (int i = 0; i < setNumber; i++) {
			ScalarVariable* sv = vars[set_valueSeq[i]];
			fmiValueReference vr = getValueReference(sv);
			const char* name = getName(sv);
			//把逗号换成句号，避免写入InfluxDB时的格式错误
			char finalName[1000];
			int k = 0;
			while (*name) {
				if (*name != ' ') {
					if (*name == ',') {
						finalName[k] = '.';
					}
					else {
						finalName[k] = *name;
					}
					k++;
				}
				name++;
			}
			//加一个"/0"进去，把后面没有填充的部分截断
			finalName[k] = 0;
			strcat(body, finalName);
			strcat(body, "=");
			char value_temp[20];
			//fmiReal value;
			//fmu->getReal(c, &vr, 1, &value);
			////strcat(body, name);
			////strcat(body, "=");
			//char value_temp[20];
			//sprintf(value_temp, "%.16g", value);
			//strcat(body, value_temp);
			fmiReal value_r;
			fmiInteger value_i;
			switch (sv->typeSpec->type) {
				case elm_Real:
					fmu->getReal(c, &vr, 1, &value_r);
					sprintf(value_temp, "%.16g", value_r);
					strcat(body, value_temp);
					break;
				case elm_Integer:
					fmu->getInteger(c, &vr, 1, &value_i);
					sprintf(value_temp, "%d", value_i);
					strcat(body, value_temp);
					break;
			}
			if (getNumber == 0) {
				if (i == setNumber - 1) {
					strcat(body, "\n");
				}
				else {
					strcat(body, ",");
				}
			}
			else {
				strcat(body, ",");
			}
		}
		for (int i = 0; i < getNumber; i++) {
			ScalarVariable* sv = vars[get_valueSeq[i]];
			fmiValueReference vr = getValueReference(sv);
			const char* name = getName(sv);
			char finalName[1000];
			int k = 0;
			while (*name) {
				if (*name != ' ') {
					if (*name == ',') {
						finalName[k] = '.';
					}
					else {
						finalName[k] = *name;
					}
					k++;
				}
				name++;
			}
			finalName[k] = 0;
			strcat(body, finalName);
			strcat(body, "=");
			char value_temp[20];
			fmiReal value_r;
			fmiInteger value_i;
			switch (sv->typeSpec->type) {
				case elm_Real:
					fmu->getReal(c, &vr, 1, &value_r);
					sprintf(value_temp, "%.16g", value_r);
					strcat(body, value_temp);
					break;
				case elm_Integer:
					fmu->getInteger(c, &vr, 1, &value_i);
					sprintf(value_temp, "%d", value_i);
					strcat(body, value_temp);
					break;
			}
			if (i == getNumber - 1) {
				strcat(body, "\n");
			}
			else {
				strcat(body, ",");
			}
		}
		sprintf(header,
			"POST /write?db=%s&u=%s&p=%s HTTP/1.1\r\nHost: influx:8086\r\nContent-Length: %zd\r\n\r\n",
			twin->database, twin->username, twin->password, strlen(body));
		ret = send(sockfd, header, strlen(header), 0);
		if (ret < 0) {
			zlog_error(zc, "write header request to InfluxDB failed\r\n");
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
			return 0;
		}
		ret = send(sockfd, body, strlen(body), 0);
		if (ret < 0) {
			zlog_error(zc, "write data body to InfluxDB failed\r\n");
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
			return 0;
		}
		//zlog_info(zc, "write initial data to InfluxDB successfully\r\n");
		ret = recv(sockfd, result, DB_BUFSIZE, 0);
		if (ret < 0) {
			zlog_error(zc, "read the result from InfluxDB failed\r\n");
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
			return 0;
		}
		result[ret] = 0; /* terminate string */
		char *buf[2], *p;
		p = NULL;
		p = strtok(result, " ");      // 分割同一字符串，第一次调用时传入字符串的首地址
		buf[0] = p;
		p = strtok(NULL, " ");      // 再次调用分割时指针要变为NULL, 也就是这里的第一个参数,分割的字符串还是str
		buf[1] = p;
		if (strcmp(buf[1], "204") == 0) {
			zlog_info(zc, "write initial data to InfluxDB successfully, status code is %s\r\n", buf[1]);
		}else {
			zlog_error(zc, "write initial data to InfluxDB failed, status code is %s\r\n", buf[1]);
			printf("Simulation failed\n");
			exit(EXIT_FAILURE);
			return 0;
		}
	}
	//simulate a step
	zlog_info(zc, "FMU simulate a step from t=%g\r\n",time);
	fmiFlag = fmu->doStep(c, time, hh, fmiTrue);
	if (fmiFlag != fmiOK) {
		zlog_error(zc, "could not simulate this step from t=%g\r\n",time);
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
		return 0;
	}
	zlog_info(zc, "FMU simulate the step from t=%g successfully\r\n",time);
	time += hh;
	//写influxdb
	zlog_info(zc, "start writing data after this step to InfluxDB\r\n");
	sprintf(body, "%s,global_id=%d timestamp=%g,", name, guid, time);
	ScalarVariable** vars = fmu->modelDescription->modelVariables;
	for (int i = 0; i < setNumber; i++) {
		ScalarVariable* sv = vars[set_valueSeq[i]];
		fmiValueReference vr = getValueReference(sv);
		const char* name = getName(sv);
		char finalName[1000];
		int k = 0;
		while (*name) {
			if (*name != ' ') {
				if (*name == ',') {
					finalName[k] = '.';
				}
				else {
					finalName[k] = *name;
				}
				k++;
			}
			name++;
		}
		finalName[k] = 0;
		strcat(body, finalName);
		strcat(body, "=");
		char value_temp[20];
		fmiReal value_r;
		fmiInteger value_i;
		switch (sv->typeSpec->type) {
		case elm_Real:
			fmu->getReal(c, &vr, 1, &value_r);
			sprintf(value_temp, "%.16g", value_r);
			strcat(body, value_temp);
			break;
		case elm_Integer:
			fmu->getInteger(c, &vr, 1, &value_i);
			sprintf(value_temp, "%d", value_i);
			strcat(body, value_temp);
			break;
		}
		if (getNumber == 0) {
			if (i == setNumber - 1) {
				strcat(body, "\n");
			}
			else {
				strcat(body, ",");
			}
		}
		else {
			strcat(body, ",");
		}
	}
	for (int i = 0; i < getNumber; i++) {
		ScalarVariable* sv = vars[get_valueSeq[i]];
		fmiValueReference vr = getValueReference(sv);
		const char* name = getName(sv);
		char finalName[1000];
		int k = 0;
		while (*name) {
			if (*name != ' ') {
				if (*name == ',') {
					finalName[k] = '.';
				}
				else {
					finalName[k] = *name;
				}
				k++;
			}
			name++;
		}
		finalName[k] = 0;
		strcat(body, finalName);
		strcat(body, "=");
		char value_temp[20];
		fmiReal value_r;
		fmiInteger value_i;
			switch (sv->typeSpec->type) {
				case elm_Real:
					fmu->getReal(c, &vr, 1, &value_r);
					sprintf(value_temp, "%.16g", value_r);
					strcat(body, value_temp);
					break;
				case elm_Integer:
					fmu->getInteger(c, &vr, 1, &value_i);
					sprintf(value_temp, "%d", value_i);
					strcat(body, value_temp);
					break;
			}
		if (i == getNumber - 1) {
			strcat(body, "\n");
		}
		else {
			strcat(body, ",");
		}
	}
	sprintf(header,
		"POST /write?db=%s&u=%s&p=%s HTTP/1.1\r\nHost: influx:8086\r\nContent-Length: %zd\r\n\r\n",
		twin->database, twin->username, twin->password, strlen(body));
	ret = send(sockfd, header, strlen(header), 0);
	if (ret < 0) {
		zlog_error(zc, "write header request to InfluxDB failed\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
		return 0;
	}
	ret = send(sockfd, body, strlen(body), 0);
	if (ret < 0) {
		zlog_error(zc, "write data body to InfluxDB failed\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
		return 0;
	}
	//zlog_info(zc, "write data to InfluxDB successfully\r\n");
	ret = recv(sockfd, result, DB_BUFSIZE, 0);
	if (ret < 0) {
		zlog_error(zc, "read the result from InfluxDB failed\r\n");
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
		return 0;
	}
	result[ret] = 0; /* terminate string */
	char *buf[2], *p;
	p = NULL;
	p = strtok(result, " ");      // 分割同一字符串，第一次调用时传入字符串的首地址
	buf[0] = p;
	p = strtok(NULL, " ");      // 再次调用分割时指针要变为NULL, 也就是这里的第一个参数,分割的字符串还是str
	buf[1] = p;
	if (strcmp(buf[1], "204") == 0) {
		zlog_info(zc, "write initial data to InfluxDB successfully, status code is %s\r\n", buf[1]);
	}
	else {
		zlog_error(zc, "write initial data to InfluxDB failed, status code is %s\r\n", buf[1]);
		printf("Simulation failed\n");
		exit(EXIT_FAILURE);
		return 0;
	}
	return time; // success
}

int main(int argc, char *argv[]) {
	TwinModel twin;
	//解析命令行参数
    parseArguments(argc, argv, &twin);	
	//influxDB配置
	twin.port = 8086;
	twin.ip_address = "127.0.0.1";
	twin.database = "rt_test";
	twin.username = "rw_db";
	twin.password = "dansan";
	//加载zlog配置文件
	rc = zlog_init("fmu_log.conf");
	//zlog初始化失败
	if (rc) {
		printf("zlog init failed\n");
		return -1;
	}
	//指定日志分类
	zc = zlog_get_category("fmu");
	if (!zc) {
		printf("get fmu category fail\n");
		zlog_fini();
		return -2;
	}

    TwinOpen(&twin);
	TwinInitialize(&twin);
	TwinSetInputs(&twin);

	////output model properties
	//ScalarVariable** vars = twin.fmu.modelDescription->modelVariables;
	//for (int k = 0; vars[k]; k++) {
	//	ScalarVariable* sv = vars[k];
	//	printf("name %d is %s\n", k, TwinGetVariableName(sv));
	//	printf("valueReference %d is %d\n", k, TwinGetVariableReference(sv));
	//	printf("valueType %d is %d\n", k, TwinGetVariableType(sv));
	//	printf("valueInit %d is %.16g\n", k, TwinGetVariableInit(sv, &twin));
	//	printf("causality %d is %d\n", k, TwinGetVariableCausality(sv));
	//}

	zlog_info(zc, "FMU Simulator: run '%s' from t=0..%g with step size h=%g\r\n", twin.fmuFileName, twin.tEnd, twin.h);
	char header[DB_BUFSIZE];
	char body[DB_BUFSIZE];
	char result[DB_BUFSIZE];
	//simulate step by step
	double time = 0;
	while (time < twin.tEnd) {
		//for test
		/*if (fabs(time - 0.3) < 1e-15) {
			printf("-------------------------------------------------equals--------------------------------------------\n");
			twin.set_value[0] = 3;
			TwinSetInputs(&twin);		
		}*/
		time = TwinSimulationByStep(&twin, time, header, body, result);
		TwinGetOutputs(&twin);
	}
	////simulate the whole process
	//TwinSimulation(&twin, header, body, result);
    printf("Simulation completed successfully\n");
	
	//TwinReset(&twin);
	//TwinInitialize(&twin);
	//TwinSimulation(&twin, header, body);

	TwinClose(&twin);
	zlog_fini();

    return EXIT_SUCCESS;
}

