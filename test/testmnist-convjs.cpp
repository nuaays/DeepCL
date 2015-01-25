//#include "OpenCLHelper.h"
//#include "ClConvolve.h"

#include <iostream>
#ifdef MPI_AVAILABLE
#include "mpi.h"
#endif

#include "BoardHelper.h"
#include "MnistLoader.h"
#include "BoardPng.h"
#include "Timer.h"
#include "NeuralNet.h"
#include "AccuracyHelper.h"
#include "stringhelper.h"
#include "FileHelper.h"
#include "StatefulTimer.h"
#include "WeightsPersister.h"
#include "test/NormalizationHelper.h"

using namespace std;

int myrank = 0;
int mysize = 1;

void loadMnist( string mnistDir, string setName, int *p_N, int *p_boardSize, float ****p_images, int **p_labels ) {
    int boardSize;
    int Nboards;
    int Nlabels;
    // images
    int ***boards = MnistLoader::loadImages( mnistDir, setName, &Nboards, &boardSize );
    int *labels = MnistLoader::loadLabels( mnistDir, setName, &Nlabels );
    if( Nboards != Nlabels ) {
         throw runtime_error("mismatch between number of boards, and number of labels " + toString(Nboards ) + " vs " +
             toString(Nlabels ) );
    }
    if( myrank == 0 ) cout << "loaded " << Nboards << " boards.  " << endl;
//    MnistLoader::shuffle( boards, labels, Nboards, boardSize );
    float ***boardsFloat = BoardsHelper::allocateBoardsFloats( Nboards, boardSize );
    BoardsHelper::copyBoards( boardsFloat, boards, Nboards, boardSize );
    BoardsHelper::deleteBoards( &boards, Nboards, boardSize );
    *p_images = boardsFloat;
    *p_labels = labels;
   
    *p_boardSize = boardSize;
    *p_N = Nboards;
}

class Config {
public:
    string dataDir = "../data/mnist";
    string trainSet = "train";
    string testSet = "t10k";
    int numTrain = 60000;
    int numTest = 10000;
    int batchSize = 128;
    int numEpochs = 12;
    int restartable = 0;
    string restartableFilename = "weights.dat";
    float learningRate = 0.002f;
    int biased = 1;
    string resultsFilename = "results.txt";
    Config() {
    }
};

float printAccuracy( string name, NeuralNet *net, float ***boards, int *labels, int batchSize, int N ) {
    int testNumRight = 0;
    net->setBatchSize( batchSize );
    int numBatches = (N + batchSize - 1 ) / batchSize;
    for( int batch = 0; batch < numBatches; batch++ ) {
        int batchStart = batch * batchSize;
        int thisBatchSize = batchSize;
        if( batch == numBatches - 1 ) {
            thisBatchSize = N - batchStart;
            net->setBatchSize( thisBatchSize );
        }
        net->propagate( &(boards[batchStart][0][0]) );
        float const*results = net->getResults();
        int thisnumright = net->calcNumRight( &(labels[batchStart]) );
        testNumRight += thisnumright;
    }
    float accuracy = ( testNumRight * 100.0f / N );
    if( myrank == 0 ) cout << name << " overall: " << testNumRight << "/" << N << " " << accuracy << "%" << endl;
    return accuracy;
}

float ***padBoards( const int N, const int boardSize, float ***unpaddedBoards, const int paddingSize ) {
    int newBoardSize = boardSize + paddingSize * 2;
    float ***newBoards = BoardsHelper::allocateBoardsFloats( N, newBoardSize );
    memset( &(newBoards[0][0][0]), 0, sizeof(float) * N * newBoardSize * newBoardSize );
    for( int n = 0; n < N; n++ ) {
        for( int i = 0; i < boardSize; i++ ) {
            for( int j = 0; j < boardSize; j++ ) {
                newBoards[n][i+paddingSize][j+paddingSize] = unpaddedBoards[n][i][j];
            }
        }
    }
    return newBoards;
}

void go(Config config) {
    Timer timer;

    int boardSize;

    float ***boardsFloat = 0;
    int *labels = 0;

    float ***boardsTest = 0;
    int *labelsTest = 0;

    int N;
    loadMnist( config.dataDir, config.trainSet, &N, &boardSize, &boardsFloat, &labels );

    int Ntest;
    loadMnist( config.dataDir, config.testSet, &Ntest, &boardSize, &boardsTest, &labelsTest );

    const int numPlanes = 1;
    const int inputCubeSize = numPlanes * boardSize * boardSize;
    float mean;
    float scaling;
    NormalizationHelper::getMeanAndMaxDev( &(boardsFloat[0][0][0]), config.numTrain * inputCubeSize, &mean, &scaling );
//    mean = 33;
//    thismax = 255;
    if( myrank == 0 ) cout << " board stats mean " << mean << " scaling " << scaling << endl;
    NormalizationHelper::normalize( &(boardsFloat[0][0][0]), config.numTrain *  inputCubeSize, mean, scaling );
    NormalizationHelper::normalize( &(boardsTest[0][0][0]), config.numTest *  inputCubeSize, mean, scaling );
    if( myrank == 0 ) timer.timeCheck("after load images");

    int numToTrain = config.numTrain;
    const int batchSize = config.batchSize;
    NeuralNet *net = NeuralNet::maker()->planes(1)->boardSize(boardSize)->instance(); // 28
    net->convolutionalMaker()->numFilters(8)->filterSize(5)->relu()->biased()->padZeros()->insert(); // 28
    net->poolingMaker()->poolingSize(2)->insert();  // 14
    net->convolutionalMaker()->numFilters(16)->filterSize(5)->relu()->biased()->padZeros()->insert(); // 14
    net->poolingMaker()->poolingSize(3)->insert(); // 5
    net->fullyConnectedMaker()->numPlanes(10)->boardSize(1)->linear()->biased(config.biased)->insert();
    net->softMaxLossMaker()->insert();
    net->setBatchSize(config.batchSize);
    net->print();

    if( config.restartable ) {
        WeightsPersister::loadWeights( config.restartableFilename, net );
    }

    timer.timeCheck("before learning start");
    StatefulTimer::timeCheck("START");
    const int totalWeightsSize = WeightsPersister::getTotalNumWeights(net);
    cout << "totalweightssize: " << totalWeightsSize << endl;
    float *weightsCopy = new float[totalWeightsSize];
    float *newWeights = new float[totalWeightsSize];
    float *weightsChange = new float[totalWeightsSize];
    float *weightsChangeReduced = new float[totalWeightsSize];
    for( int epoch = 0; epoch < config.numEpochs; epoch++ ) {
        int trainTotalNumber = 0;
        int trainNumRight = 0;
        int numBatches = ( config.numTrain + config.batchSize - 1 ) / config.batchSize;
        int eachNodeBatchSize = config.batchSize / mysize;
        int thisNodeBatchSize = eachNodeBatchSize;
        if( myrank == mysize - 1 ) {
            thisNodeBatchSize = config.batchSize - ( mysize - 1 ) * eachNodeBatchSize;
        }
        net->setBatchSize( thisNodeBatchSize );
        float loss = 0;
        for( int batch = 0; batch < numBatches; batch++ ) {
            int batchStart = batch * config.batchSize;
            int thisBatchSize = config.batchSize;
            int nodeBatchStart = batchStart + myrank * eachNodeBatchSize;
            if( batch == numBatches - 1 ) {
                thisBatchSize = config.numTrain - batchStart;
                eachNodeBatchSize = thisBatchSize / mysize;
                nodeBatchStart = batchStart + myrank * eachNodeBatchSize;
                thisNodeBatchSize = eachNodeBatchSize;
                if( myrank == mysize - 1 ) {
                    thisNodeBatchSize = thisBatchSize - ( mysize - 1 ) * eachNodeBatchSize;
                }
                net->setBatchSize( thisNodeBatchSize );
            }
            #ifdef MPI_AVAILABLE
            if( mysize > 0 ) {
                StatefulTimer::timeCheck("copyNetWeightsToArray START");
                WeightsPersister::copyNetWeightsToArray( net, weightsCopy );
                StatefulTimer::timeCheck("copyNetWeightsToArray END");
            }
            #endif
            net->propagate( &(boardsFloat[nodeBatchStart][0][0]) );
            net->backPropFromLabels( config.learningRate, &(labels[nodeBatchStart]) );
            trainTotalNumber += thisNodeBatchSize;
            trainNumRight += net->calcNumRight( &(labels[nodeBatchStart]) );
            loss += net->calcLossFromLabels( &(labels[nodeBatchStart]) );
            // share out the weights... just average them?
            // for each weight, wnew = wold + dw
            // if multiple changes, wnew = wold + dw1 + dw2 + dw3 + ...
            // if each node doing it, then wnew1 = wold + dw1; wnew2 = wold + dw2
            // wnew1 + wnew2 = wold * 2 + dw1 + dw2
            // we want: wnew = wold + dw1 + dw2 = wnew1 + wnew2 - wold
            // seems like we should keep a copy of the old weights, otherwise cannot compute
            #ifdef MPI_AVAILABLE
            if( mysize > 0 ) {
                StatefulTimer::timeCheck("allreduce START");
                WeightsPersister::copyNetWeightsToArray( net, newWeights );
                StatefulTimer::timeCheck("allreduce done copyNetWeightsToArray");
                if( myrank == 0 ) {
                    for( int i = 0; i < totalWeightsSize; i++ ) {
                        weightsChange[i] = newWeights[i];
                    }
                } else {
                    for( int i = 0; i < totalWeightsSize; i++ ) {
                        weightsChange[i] = newWeights[i] - weightsCopy[i];
                    }
                }
                MPI_Allreduce( weightsChange, weightsChangeReduced, totalWeightsSize, MPI_FLOAT, MPI_SUM, MPI_COMM_WORLD );
                StatefulTimer::timeCheck("allreduce done Allreduce");
                WeightsPersister::copyArrayToNetWeights( weightsChangeReduced, net );
                StatefulTimer::timeCheck("allreduce END");
            }
            #endif            
        }
        StatefulTimer::dump(true);
        if( myrank == 0 ) cout << "       loss L: " << loss << endl;
        if( myrank == 0 ) timer.timeCheck("after epoch " + toString(epoch) );
//        net->print();
        if( myrank == 0 ) std::cout << "train accuracy: " << trainNumRight << "/" << trainTotalNumber << " " << (trainNumRight * 100.0f/ trainTotalNumber) << "%" << std::endl;
        if( myrank == 0 ) printAccuracy( "test", net, boardsTest, labelsTest, batchSize, config.numTest );
        if( myrank == 0 ) timer.timeCheck("after tests");
        if( config.restartable ) {
            WeightsPersister::persistWeights( config.restartableFilename, net );
        }
    }
    delete[] weightsCopy;

    if( myrank == 0 ) printAccuracy( "test", net, boardsTest, labelsTest, batchSize, config.numTest );
    if( myrank == 0 ) timer.timeCheck("after tests");

    int numTestBatches = ( config.numTest + config.batchSize - 1 ) / config.batchSize;
    int totalNumber = 0;
    int totalNumRight = 0;
    net->setBatchSize( config.batchSize );
    for( int batch = 0; batch < numTestBatches; batch++ ) {
        int batchStart = batch * config.batchSize;
        int thisBatchSize = config.batchSize;
        if( batch == numTestBatches - 1 ) {
            thisBatchSize = config.numTest - batchStart;
            net->setBatchSize( thisBatchSize );
        }
        net->propagate( &(boardsTest[batchStart][0][0]) );
        float const*resultsTest = net->getResults();
        totalNumber += thisBatchSize;
        totalNumRight += net->calcNumRight( &(labelsTest[batchStart]) );
    }
    if( myrank == 0 ) cout << "test accuracy : " << totalNumRight << "/" << totalNumber << endl;

    delete net;

    delete[] labelsTest;
//    BoardsHelper::deleteBoards( &boardsTest, Ntest, boardSize );

    delete[] labels;
//    BoardsHelper::deleteBoards( &boardsFloat, N, boardSize );
}

int main( int argc, char *argv[] ) {
    #ifdef MPI_AVAILABLE
        MPI_Init( &argc, &argv );
        MPI_Comm_rank( MPI_COMM_WORLD, &myrank );
        MPI_Comm_size( MPI_COMM_WORLD, &mysize );
    #endif

    Config config;
    if( myrank == 0 ) {
        if( argc == 2 && ( string(argv[1]) == "--help" || string(argv[1]) == "--?" || string(argv[1]) == "-?" || string(argv[1]) == "-h" ) ) {
            cout << "Usage: " << argv[0] << " [key]=[value] [[key]=[value]] ..." << endl;
            cout << "Possible key=value pairs:" << endl;
            cout << "    datadir=[data directory] (" << config.dataDir << ")" << endl;
            cout << "    trainset=[train|t10k|other set name] (" << config.trainSet << ")" << endl;
            cout << "    testset=[train|t10k|other set name] (" << config.testSet << ")" << endl;
            cout << "    numtrain=[num training examples] (" << config.numTrain << ")" << endl;
            cout << "    numtest=[num test examples] (" << config.numTest << ")" << endl;
            cout << "    batchsize=[batch size] (" << config.batchSize << ")" << endl;
            cout << "    numepochs=[number epochs] (" << config.numEpochs << ")" << endl;
            cout << "    biased=[0|1] (" << config.biased << ")" << endl;
            cout << "    learningrate=[learning rate, a float value] (" << config.learningRate << ")" << endl;
            cout << "    restartable=[weights are persistent?] (" << config.restartable << ")" << endl;
            cout << "    restartablefilename=[filename to store weights] (" << config.restartableFilename << ")" << endl;
            cout << "    resultsfilename=[filename to store results] (" << config.resultsFilename << ")" << endl;
        } 
    }
    for( int i = 1; i < argc; i++ ) {
       vector<string> splitkeyval = split( argv[i], "=" );
       if( splitkeyval.size() != 2 ) {
            if( myrank == 0  ){
                cout << "Usage: " << argv[0] << " [key]=[value] [[key]=[value]] ..." << endl;
            }
            #ifdef MPI_AVAILABLE
            MPI_Finalize();
            #endif
            exit(1);
       } else {
           string key = splitkeyval[0];
           string value = splitkeyval[1];
           if( key == "datadir" ) config.dataDir = value;
           if( key == "trainset" ) config.trainSet = value;
           if( key == "testset" ) config.testSet = value;
           if( key == "numtrain" ) config.numTrain = atoi(value);
           if( key == "numtest" ) config.numTest = atoi(value);
           if( key == "batchsize" ) config.batchSize = atoi(value);
           if( key == "numepochs" ) config.numEpochs = atoi(value);
           if( key == "biased" ) config.biased = atoi(value);
           if( key == "learningrate" ) config.learningRate = atof(value);
           if( key == "restartable" ) config.restartable = atoi(value);
           if( key == "restartablefilename" ) config.restartableFilename = value;
           if( key == "resultsfilename" ) config.resultsFilename = value;
       }
    }
    go( config );
    #ifdef MPI_AVAILABLE
    MPI_Finalize();
    #endif
}


